"""Temperature tracking plugin for paired ESP sensor devices."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from flask import Blueprint, jsonify, render_template, request, abort
import logging

from services.data_manager import data_manager

TEMPERATURE_PACKET_TYPE = 0x55  # Plugin packet type for temperature readings
FILE_TEMPERATURE_SENSORS = 'temperature_sensors.json'
FILE_TEMPERATURE_READINGS = 'temperature_readings.json'


@dataclass
class TemperatureTrackingPlugin:
    """Tracks temperature sensors and records readings from plugin-owned packets."""

    options: dict[str, Any] = field(default_factory=dict)
    context: Any = None
    manifest: Any = None

    def __post_init__(self) -> None:
        self.blueprint = Blueprint(
            'temperature_tracking',
            __name__,
            url_prefix='/plugins/temperature',
            template_folder='templates',
            static_folder='static',
        )
        self.blueprint.add_url_rule('/status', 'status', self.status)
        self.blueprint.add_url_rule('/', 'index', self.index)
        self.blueprint.add_url_rule('/sensors', 'sensors', self.sensors)
        self.blueprint.add_url_rule('/readings', 'readings', self.readings)

        # API endpoints for device management
        self.blueprint.add_url_rule('/api/devices/<device_id>/rename', 'api_device_rename', self.api_device_rename, methods=['POST'])
        self.blueprint.add_url_rule('/api/devices/<device_id>/configure', 'api_device_configure', self.api_device_configure, methods=['POST'])
        self.blueprint.add_url_rule('/api/devices/<device_id>', 'api_device_delete', self.api_device_delete, methods=['DELETE'])
        self._sensors = self._read_json(FILE_TEMPERATURE_SENSORS)
        self._readings = self._read_json(FILE_TEMPERATURE_READINGS)
        
        # Read from context (initialized in register/start)
        self.plugin_uuid: str | None = None
        self.plugin_device_type: int = 0

    def register(self, app, context=None):
        self.context = context or self.context
        self._init_from_context()
        app.register_blueprint(self.blueprint)

    def start(self, context=None):
        self.context = context or self.context
        self._init_from_context()
        self._subscribe_to_events()
        self._log('Temperature tracking plugin started.')

    def _subscribe_to_events(self):
        """Subscribe to core events for device management."""
        if self.context and hasattr(self.context, 'subscribe'):
            try:
                self.context.subscribe('device_renamed', self.on_device_renamed)
                self._log('Subscribed to device_renamed events')
            except Exception as e:
                self._log(f'Failed to subscribe to device_renamed: {e}')
    
    def _init_from_context(self):
        """Initialize plugin UUID and device type from context manifest."""
        plugin_uuid = None

        # Preferred source: manifest injected by plugin manager at create/register/start hooks.
        if self.manifest is not None:
            if isinstance(self.manifest, dict):
                plugin_uuid = self.manifest.get('plugin_id') or self.manifest.get('id')
            else:
                plugin_uuid = getattr(self.manifest, 'plugin_id', None) or getattr(self.manifest, 'id', None)

        self.plugin_uuid = str(plugin_uuid).strip() if plugin_uuid else None

    def stop(self, context=None):
        self._log('Temperature tracking plugin stopped.')

    def on_plugin_hello(self, event_name: str, payload: dict[str, Any], host=None):
        """Handle plugin_hello event from network server.
        
        Plugin devices advertise with 0xFE marker and plugin UUID.
        This method handles pairing discovery and MAC binding.
        
        Args:
            event_name: Event name ('plugin_hello')
            payload: Event payload containing device_mac, plugin_uuid, plugin_device_type, rssi, etc.
            host: Plugin host context
        """
        if not self.plugin_uuid:
            self._log('Plugin UUID not initialized, ignoring plugin_hello')
            return
            
        device_mac = payload.get('device_mac')
        plugin_uuid = payload.get('plugin_uuid', '')
        plugin_device_type = payload.get('plugin_device_type', 0)
        rssi = payload.get('rssi', 0)
        version = payload.get('version', '0.0.0')
        platform = payload.get('platform', 'esp32')
        sender_ip = payload.get('sender_ip')
        gateway_radio_mac = payload.get('gateway_radio_mac')
        is_paired = payload.get('is_paired', False)
        
        # UUID validation: normalize and compare
        if plugin_uuid.replace('-', '').lower() != self.plugin_uuid.replace('-', '').lower():
            self._log(f'Ignoring plugin_hello for different UUID: {plugin_uuid}')
            return

        if not is_paired:
            self._log(f'Ignoring unpaired plugin HELLO for {device_mac} (UUID {plugin_uuid})')
            return

        normalized_gateway_mac = gateway_radio_mac.upper() if isinstance(gateway_radio_mac, str) and gateway_radio_mac else None
        self._upsert_sensor(
            device_mac,
            rssi=rssi,
            version=version,
            platform=platform,
            state='paired',
            last_seen_gateway=normalized_gateway_mac,
        )
        self._log(f'Temperature sensor registered/updated: {device_mac}')

        if host and hasattr(host, 'publish'):
            try:
                plugin_id = payload.get('plugin_id', plugin_uuid)
                host.publish('temperature_sensor_paired', {
                    'sensor_mac': device_mac,
                    'plugin_id': plugin_id,
                    'rssi': rssi,
                    'version': version,
                    'platform': platform,
                })
            except Exception:
                pass

    def on_packet_received(self, event_name: str, payload: dict[str, Any], host=None):
        """Handle packet_received events from network server.
        
        Routes TEMPERATURE_PACKET_TYPE readings to handler.
        Validates packet belongs to this plugin via MAC routing.
        
        Args:
            event_name: Event name ('packet_received')
            payload: Packet payload with type, payload, source_mac, sender_ip, gateway_radio_mac
            host: Plugin host context
        """
        packet_type = payload.get('type')
        packet_payload = payload.get('payload') or b''
        source_mac = payload.get('source_mac')
        sender_ip = payload.get('sender_ip')
        gateway_radio_mac = payload.get('gateway_radio_mac')

        if not self.plugin_uuid:
            return
        
        # Check if this device is routed to our plugin
        plugin_manager = self._get_plugin_manager(host)
        if not plugin_manager:
            return
            
        plugin_for_mac = plugin_manager.get_plugin_for_mac(source_mac)
        if plugin_for_mac and plugin_for_mac.replace('-', '').lower() != self.plugin_uuid.replace('-', '').lower():
            self._log(f'Ignoring packet from {source_mac} handled by different plugin {plugin_for_mac}')
            return

        if packet_type == TEMPERATURE_PACKET_TYPE:
            self._handle_temperature_reading(source_mac, packet_payload, sender_ip, gateway_radio_mac, host)
    
    def _handle_temperature_reading(self, sensor_mac: str, payload: bytes, sender_ip: str, gateway_radio_mac: str | None, host=None):
        normalized_gateway_mac = gateway_radio_mac.upper() if isinstance(gateway_radio_mac, str) and gateway_radio_mac else None
        self._log(f'Received temperature packet from {sensor_mac} via gateway {normalized_gateway_mac}')
        if len(payload) < 7 or not sensor_mac:
            return

        temp_c_x10, humidity_x10, battery_mv, flags = struct.unpack('<hHHB', payload[:7])
        reading = {
            'sensor_mac': sensor_mac,
            'temperature_c': temp_c_x10 / 10.0,
            'battery_mv': battery_mv,
            'flags': flags,
            'timestamp': datetime.now(timezone.utc).isoformat(),
            'sender_ip': sender_ip,
        }
        if humidity_x10 != 0xFFFF:
            reading['humidity_pct'] = humidity_x10 / 10.0

        self._readings.append(reading)
        history_limit = int(self.options.get('history_limit', 100))
        self._readings = self._readings[-history_limit:]
        self._write_json(FILE_TEMPERATURE_READINGS, self._readings)

        self._upsert_sensor(
            sensor_mac,
            temperature_c=reading['temperature_c'],
            humidity_pct=reading.get('humidity_pct'),
            battery_mv=battery_mv,
            flags=flags,
            last_seen_gateway=normalized_gateway_mac,
        )

        if host and hasattr(host, 'publish'):
            host.publish('temperature_reading', reading)

    def status(self):
        return jsonify({
            'enabled': True,
            'sensors': self._sensors,
            'readings': self._readings[-10:],
            'options': self.options,
        })

    def sensors(self):
        return jsonify(self._sensors)

    def readings(self):
        return jsonify(self._readings)

    def index(self):
        """Render plugin home page showing sensors and readings."""
        return render_template('temperature.html')

    def _upsert_sensor(self, sensor_mac: str, **updates: Any) -> dict[str, Any]:
        sensor = next((item for item in self._sensors if item.get('mac_address', '').upper() == sensor_mac.upper()), None)
        if sensor is None:
            sensor = {
                'id': sensor_mac.replace(':', '').lower(),
                'name': f'Temp Sensor {sensor_mac[-8:]}',
                'mac_address': sensor_mac.upper(),
                'state': 'unknown',
                'configured': False,
                'config': {
                    'room_id': None,
                    'enabled': True,
                    'min_temperature_c': None,
                    'max_temperature_c': None,
                },
                'last_seen': None,
                'last_seen_gateway': None,
                'rssi': None,
                'version': '0.0.0',
                'platform': None,
                'last_battery_mv': None,
            }
            self._sensors.append(sensor)

        for key, value in updates.items():
            if value is not None:
                sensor[key] = value

        sensor['last_seen'] = datetime.now(timezone.utc).isoformat()
        self._write_json(FILE_TEMPERATURE_SENSORS, self._sensors)
        return sensor

    def _read_json(self, filename: str) -> list[dict[str, Any]]:
        data = data_manager.read_json(filename, default=[])
        return data if isinstance(data, list) else []

    def _write_json(self, filename: str, data: Any) -> None:
        data_manager.write_json(filename, data)

    def on_device_renamed(self, event_name: str, payload: dict[str, Any], host=None):
        """Handle device_renamed events from the core pairing manager.
        
        Args:
            event_name: Event name ('device_renamed')
            payload: Event payload containing device_id, plugin_id, old_name, new_name, device_type
            host: Plugin host context
        """
        plugin_id = payload.get('plugin_id')
        if plugin_id and plugin_id != self.plugin_uuid:
            return

        device_id = payload.get('device_id')
        new_name = payload.get('new_name')
        
        if not device_id or not new_name:
            return

        sensor = self._find_sensor_by_id(device_id)
        if sensor:
            old_name = sensor.get('name')
            sensor['name'] = new_name
            self._write_json(FILE_TEMPERATURE_SENSORS, self._sensors)
            self._log(f'Device renamed: {old_name} -> {new_name}')

    def _find_sensor_by_id(self, sensor_id: str) -> dict[str, Any] | None:
        sid = sensor_id.lower()
        return next((s for s in self._sensors if s.get('id', '').lower() == sid or s.get('mac_address', '').replace(':', '').lower() == sid or s.get('mac_address', '').lower() == sid), None)

    def api_device_rename(self, device_id: str):
        """Rename a device (called by core pairing manager and UI)."""
        payload = request.get_json(silent=True) or {}
        name = payload.get('name', '').strip()
        if not name:
            return jsonify({'success': False, 'error': 'Name is required'}), 400

        sensor = self._find_sensor_by_id(device_id)
        if sensor is None:
            return jsonify({'success': False, 'error': 'Device not found'}), 404

        sensor['name'] = name
        self._write_json(FILE_TEMPERATURE_SENSORS, self._sensors)
        return jsonify({'success': True, 'device': sensor})

    def api_device_configure(self, device_id: str):
        """Configure a device (called by core and UI)."""
        payload = request.get_json(silent=True) or {}
        sensor = self._find_sensor_by_id(device_id)
        if sensor is None:
            return jsonify({'success': False, 'error': 'Device not found'}), 404

        cfg = sensor.get('config', {}) or {}
        if 'enabled' in payload:
            cfg['enabled'] = bool(payload.get('enabled'))
        if 'min_temperature_c' in payload:
            cfg['min_temperature_c'] = payload.get('min_temperature_c')
        if 'max_temperature_c' in payload:
            cfg['max_temperature_c'] = payload.get('max_temperature_c')

        sensor['config'] = cfg
        sensor['configured'] = True
        self._write_json(FILE_TEMPERATURE_SENSORS, self._sensors)
        return jsonify({'success': True, 'device': sensor})

    def api_device_delete(self, device_id: str):
        """Delete a device (called by core and UI)."""
        sensor = self._find_sensor_by_id(device_id)
        if sensor is None:
            return jsonify({'success': False, 'error': 'Device not found'}), 404

        try:
            sensor_mac = str(sensor.get('mac_address') or '').upper()
            self._sensors = [s for s in self._sensors if s is not sensor]
            self._write_json(FILE_TEMPERATURE_SENSORS, self._sensors)

            # Keep pairing history in sync when a plugin sensor is deleted.
            pairing_manager = self._get_pairing_manager(self.context)
            if pairing_manager and sensor_mac:
                try:
                    pairing_manager.remove_paired_device(sensor_mac)
                except Exception as e:
                    self._log(f'Failed to remove {sensor_mac} from pairing history: {e}')

            return jsonify({'success': True})
        except Exception as e:
            return jsonify({'success': False, 'error': str(e)}), 500

    def _get_network_server(self, host=None):
        if host is not None:
            try:
                return host.require('network_server')
            except Exception:
                pass
        return None

    def _get_pairing_manager(self, host=None):
        if host is not None:
            try:
                return host.require('pairing_manager')
            except Exception:
                pass
        return None

    def _get_plugin_manager(self, host=None):
        if host is not None:
            try:
                return host.require('plugin_manager')
            except Exception:
                pass
        return None

    @staticmethod
    def _parse_rssi(rssi_byte: int) -> int:
        return rssi_byte - 256 if rssi_byte > 127 else rssi_byte

    def _log(self, message: str) -> None:
        logger = getattr(self.context, 'logger', None) if self.context is not None else None
        if logger is None:
            logger = logging.getLogger('plugins.temperature_tracking')
        try:
            logger.info(message)
        except Exception:
            pass


def create_plugin(options=None, context=None, manifest=None, host=None, **_kwargs):
    return TemperatureTrackingPlugin(options=options or {}, context=context or host, manifest=manifest)
