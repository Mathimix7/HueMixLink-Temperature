"""Temperature tracking plugin for paired ESP sensor devices."""
from __future__ import annotations

import binascii
import json
import struct
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from flask import Blueprint, jsonify, render_template, request
import logging

from services.battery import calculate_battery_percent

from .storage import TemperatureStorage, default_device_config

TEMPERATURE_PACKET_TYPE = 0x55  # Plugin packet type for temperature readings
PKT_TIME_REQUEST = 0x56
PKT_TIME_RESPONSE = 0x57
PKT_TEMP_CONFIG = 0x58
PKT_TEMP_SECONDARY = 0x59
MAX_SHORT_NAME_LENGTH = 5
REPORT_INTERVAL_MIN = 60       # 1 minute
REPORT_INTERVAL_MAX = 86400    # 24 hours
DEFAULT_REPORT_INTERVAL = 600  # 10 minutes
    

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
        self.blueprint.add_url_rule('/graphs', 'graphs', self.graphs)
        self.blueprint.add_url_rule('/sensors', 'sensors', self.sensors)
        self.blueprint.add_url_rule('/readings', 'readings', self.readings)
        self.blueprint.add_url_rule('/readings/aggregate', 'readings_aggregate', self.readings_aggregate)

        # API endpoints for device management
        self.blueprint.add_url_rule('/api/sensor_states', 'api_sensor_states', self.api_sensor_states)
        self.blueprint.add_url_rule('/api/devices/<device_id>/rename', 'api_device_rename', self.api_device_rename, methods=['POST'])
        self.blueprint.add_url_rule('/api/devices/<device_id>/configure', 'api_device_configure', self.api_device_configure, methods=['POST'])
        self.blueprint.add_url_rule('/api/devices/<device_id>', 'api_device_delete', self.api_device_delete, methods=['DELETE'])
        self._storage = TemperatureStorage()
        
        # Read from context (initialized in register/start)
        self.plugin_uuid: str | None = None

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

    @staticmethod
    def _validate_time_string(value: Any, field_name: str) -> tuple[str | None, str | None]:
        if not isinstance(value, str):
            return None, f'{field_name} must be in HH:MM format'

        candidate = value.strip()
        try:
            datetime.strptime(candidate, '%H:%M')
        except ValueError:
            return None, f'{field_name} must be in HH:MM format'
        return candidate, None

    @staticmethod
    def _validate_brightness_percent(value: Any, field_name: str) -> tuple[int | None, str | None]:
        try:
            brightness = int(value)
        except (TypeError, ValueError):
            return None, f'{field_name} must be a number between 0 and 100'

        if brightness < 0 or brightness > 100:
            return None, f'{field_name} must be between 0 and 100'
        return brightness, None

    @staticmethod
    def _validate_report_interval(value: Any) -> tuple[int, str | None]:
        try:
            interval = int(value)
        except (TypeError, ValueError):
            return DEFAULT_REPORT_INTERVAL, None

        if interval < REPORT_INTERVAL_MIN:
            return 0, f'Report interval must be at least {REPORT_INTERVAL_MIN} seconds ({REPORT_INTERVAL_MIN // 60} minute)'
        if interval > REPORT_INTERVAL_MAX:
            return 0, f'Report interval must be at most {REPORT_INTERVAL_MAX} seconds ({REPORT_INTERVAL_MAX // 3600} hours)'
        return interval, None

    @staticmethod
    def _validate_short_name(value: Any, field_name: str) -> tuple[str, str | None]:
        if value is None:
            return '', None
        if not isinstance(value, str):
            return '', f'{field_name} must be text'

        candidate = value.strip()
        if len(candidate) > MAX_SHORT_NAME_LENGTH:
            return '', f'{field_name} must be 5 characters or fewer'
        return candidate, None

    def _build_device_config(self, sensor: dict[str, Any], payload: dict[str, Any]) -> tuple[dict[str, Any] | None, str | None]:
        current_config = sensor.get('config') if isinstance(sensor.get('config'), dict) else default_device_config()
        current_brightness = current_config.get('brightness') if isinstance(current_config.get('brightness'), dict) else {}
        current_secondary = current_config.get('secondary') if isinstance(current_config.get('secondary'), dict) else {}

        brightness_payload = payload.get('brightness') if isinstance(payload.get('brightness'), dict) else {}

        day_start, error = self._validate_time_string(
            brightness_payload.get('day_start', current_brightness.get('day_start', '07:00')),
            'Day start time',
        )
        if error:
            return None, error

        night_start, error = self._validate_time_string(
            brightness_payload.get('night_start', current_brightness.get('night_start', '21:00')),
            'Night start time',
        )
        if error:
            return None, error

        day_brightness, error = self._validate_brightness_percent(
            brightness_payload.get('day', current_brightness.get('day', 100)),
            'Day brightness',
        )
        if error:
            return None, error

        night_brightness, error = self._validate_brightness_percent(
            brightness_payload.get('night', current_brightness.get('night', 25)),
            'Night brightness',
        )
        if error:
            return None, error

        primary_short_name, error = self._validate_short_name(
            payload.get('primary_short_name', current_config.get('primary_short_name', '')),
            'Primary short name',
        )
        if error:
            return None, error

        secondary_payload = payload.get('secondary') if isinstance(payload.get('secondary'), dict) else {}
        secondary_enabled = bool(secondary_payload.get('enabled', current_secondary.get('enabled', False)))
        secondary_short_name, error = self._validate_short_name(
            secondary_payload.get('short_name', current_secondary.get('short_name', '')),
            'Secondary short name',
        )
        if error:
            return None, error

        secondary_sensor_id = secondary_payload.get('sensor_id', current_secondary.get('sensor_id'))
        normalized_secondary_sensor_id: str | None = None
        if secondary_enabled:
            secondary_sensor_id = str(secondary_sensor_id or '').strip()
            if not secondary_sensor_id:
                return None, 'Secondary display sensor is required when the secondary display is enabled'

            secondary_sensor = self._find_sensor_by_id(secondary_sensor_id)
            if secondary_sensor is None:
                return None, 'Secondary display sensor was not found'

            if str(secondary_sensor.get('id') or '').lower() == str(sensor.get('id') or '').lower():
                return None, 'Secondary display must use a different sensor'

            normalized_secondary_sensor_id = str(secondary_sensor.get('id') or secondary_sensor_id).strip()
        else:
            secondary_short_name = ''

        report_interval, error = self._validate_report_interval(
            payload.get('report_interval_seconds', current_config.get('report_interval_seconds', DEFAULT_REPORT_INTERVAL)),
        )
        if error:
            return None, error

        return {
            '__replace__': True,
            'report_interval_seconds': report_interval,
            'brightness': {
                'day_start': day_start,
                'day': day_brightness,
                'night_start': night_start,
                'night': night_brightness,
            },
            'primary_short_name': primary_short_name,
            'secondary': {
                'enabled': secondary_enabled,
                'sensor_id': normalized_secondary_sensor_id,
                'short_name': secondary_short_name,
            },
        }, None

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
            device_type=plugin_device_type,
        )
        self._log(f'Temperature sensor registered/updated: {device_mac}')
        self._check_and_send_pending_config(device_mac)
        if plugin_device_type != 1:
            self._forward_secondary_reading_on_startup(device_mac, host)

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

        # Edit to not just check for device not being part of the plguin, but also checking the device IS part of the plugin. 
        plugin_for_mac = plugin_manager.get_plugin_for_mac(source_mac)
        if not plugin_for_mac or plugin_for_mac.replace('-', '').lower() != self.plugin_uuid.replace('-', '').lower():
            self._log(f'Ignoring packet from {source_mac} handled by different plugin {plugin_for_mac}')
            return
        
        was_known_sensor = self._find_sensor_by_id(source_mac)
        if not was_known_sensor or was_known_sensor.get('state', 'unknown') != 'paired':
            self._request_sensor_hello(host, source_mac, sender_ip)

        if packet_type == TEMPERATURE_PACKET_TYPE:
            self._handle_temperature_reading(source_mac, packet_payload, sender_ip, gateway_radio_mac, host)
        elif packet_type == PKT_TIME_REQUEST:
            self._handle_time_request(source_mac, sender_ip, gateway_radio_mac, host)
    
    def _handle_time_request(self, sensor_mac: str, sender_ip: str | None, gateway_radio_mac: str | None, host=None):
        """Respond to a device request for the current time."""
        if not sensor_mac or not host or sender_ip is None:
            return

        try:
            network_server = host.require('network_server')
        except Exception:
            return

        try:
            server_time = int(datetime.now(timezone.utc).timestamp())
            tz_offset_seconds = int(datetime.now().astimezone().utcoffset().total_seconds())
            msg_id = network_server.get_message_id()
            packet = self.encode_time_response(network_server, sensor_mac, server_time, tz_offset_seconds, msg_id)
            network_server.send_raw_packet_to_device(sensor_mac, packet, wait_for_delivery=True, msg_id=msg_id, gateway_preference=gateway_radio_mac)
            self._log(f'Sent time response to {sensor_mac}: {server_time}')
            
            # Check pending config for all sensors; skip screen mirroring for battery
            sensor = self._find_sensor_by_id(sensor_mac)
            is_battery = sensor and sensor.get('device_type') == 1
            self._check_and_send_pending_config(sensor_mac)
            if not is_battery:
                self._forward_secondary_reading_on_startup(sensor_mac, host)
        except Exception as exc:
            self._log(f'Failed to respond to time request from {sensor_mac}: {exc}')

    def encode_time_response(self, network_server, target_mac: str, epoch_seconds: int, tz_offset_seconds: int, msg_id: int = 0) -> bytes:
        """Encode a time response packet containing a Unix epoch timestamp."""
        pkt_type = PKT_TIME_RESPONSE
        src_mac = b'\x00' * 6
        
        tgt_mac = binascii.unhexlify(target_mac.replace(":", ""))
        payload = struct.pack("<Ii", epoch_seconds, tz_offset_seconds)
        payload += b'\x00' * (185 - len(payload))
        signature = network_server.encoder._calculate_hash(payload)
        packet = struct.pack("<BI", pkt_type, signature) + src_mac + tgt_mac + struct.pack("<B", msg_id) + payload
        return packet

    def _handle_temperature_reading(self, sensor_mac: str, payload: bytes, sender_ip: str, gateway_radio_mac: str | None, host=None):
        normalized_gateway_mac = gateway_radio_mac.upper() if isinstance(gateway_radio_mac, str) and gateway_radio_mac else None
        self._log(f'Received temperature packet from {sensor_mac} via gateway {normalized_gateway_mac}')
        if len(payload) < 7 or not sensor_mac:
            return

        temp_c_x10, humidity_x10, battery_mv, flags = struct.unpack('<hHHB', payload[:7])
        is_cr123a = bool(flags & 0x80)
        battery_type = 'cr123a' if is_cr123a else 'li_ion'
        battery_percent = calculate_battery_percent(battery_mv, battery_type)
        timestamp = datetime.now(timezone.utc).isoformat()
        reading = {
            'sensor_mac': sensor_mac,
            'temperature_c': temp_c_x10 / 10.0,
            'battery_mv': battery_mv,
            'battery_percent': battery_percent,
            'is_cr123a': is_cr123a,
            'flags': flags,
            'timestamp': timestamp,
            'gateway_radio_mac': normalized_gateway_mac,
        }
        if humidity_x10 != 0xFFFF:
            reading['humidity_pct'] = humidity_x10 / 10.0

        # Derive device type from flags byte (0x01=OLED, 0x02=battery)
        # so deleted sensors get re-created with the correct type.
        device_type = 1 if (flags & 0x02) else 0

        self._upsert_sensor(
            sensor_mac,
            temperature_c=reading['temperature_c'],
            humidity_pct=reading.get('humidity_pct'),
            battery_mv=battery_mv,
            battery_percent=battery_percent,
            battery_last_updated=timestamp,
            flags=flags,
            device_type=device_type,
            last_seen_gateway=normalized_gateway_mac,
        )

        self._storage.add_reading(
            sensor_mac,
            temperature_c=reading['temperature_c'],
            humidity_pct=reading.get('humidity_pct'),
            battery_mv=battery_mv,
            battery_percent=battery_percent,
            flags=flags,
            gateway_radio_mac=normalized_gateway_mac,
            timestamp=timestamp,
        )

        if host and hasattr(host, 'publish'):
            host.publish('temperature_reading', reading)

        self._forward_secondary_reading_if_needed(sensor_mac, reading, host)
        self._check_and_send_pending_config(sensor_mac)

    def get_ota_devices(self) -> list[dict]:
        sensors = self._storage.list_sensors()
        return [
            {
                'mac_address': s['mac_address'],
                'version': s.get('version', '0.0.0'),
                'platform': s.get('platform', 'esp32'),
                'last_gateway_mac': s.get('last_seen_gateway'),
                'name': s.get('name', 'Plugin Device'),
                'device_type': s.get('device_type', 0),
            }
            for s in sensors if s.get('mac_address')
        ]

    def status(self):
        return jsonify({
            'enabled': True,
            'sensors': self._storage.list_sensors(),
            'readings': self._storage.list_readings(limit=10),
            'options': self.options,
        })

    def sensors(self):
        return jsonify(self._storage.list_sensors())

    def readings(self):
        limit = request.args.get('limit', 100)
        try:
            limit_value = int(limit)
        except (TypeError, ValueError):
            limit_value = 100
        sensor_id = request.args.get('sensor_id')
        start = request.args.get('start')
        end = request.args.get('end')
        return jsonify(self._storage.list_readings(
            limit=limit_value,
            sensor_id=sensor_id,
            start=start,
            end=end,
        ))

    def readings_aggregate(self):
        sensor_id = request.args.get('sensor_id')
        if not sensor_id:
            return jsonify({'error': 'sensor_id is required'}), 400
        start = request.args.get('start')
        end = request.args.get('end')
        bucket = request.args.get('bucket', 'hour')
        if bucket not in ('hour', 'day'):
            bucket = 'hour'
        return jsonify(self._storage.get_readings_aggregate(
            sensor_id=sensor_id,
            start=start,
            end=end,
            bucket=bucket,
        ))

    def graphs(self):
        """Render graphs page showing temperature/humidity charts."""
        sensors = self._storage.list_sensors()
        sensors_json = json.dumps(sensors)
        return render_template('temperature_graphs.html', sensors_json=sensors_json)

    def index(self):
        """Render plugin home page showing sensors and readings."""
        return render_template('temperature.html')

    def _upsert_sensor(self, sensor_mac: str, **updates: Any) -> dict[str, Any]:
        updates = dict(updates)
        updates.setdefault('last_seen', datetime.now(timezone.utc).isoformat())
        return self._storage.upsert_sensor(sensor_mac, **updates)

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
            self._storage.update_sensor(device_id, name=new_name)
            self._log(f'Device renamed: {old_name} -> {new_name}')

    def _find_sensor_by_id(self, sensor_id: str) -> dict[str, Any] | None:
        return self._storage.get_sensor(sensor_id)

    def api_sensor_states(self):
        """Return dict of sensor_id -> updated_at timestamp for polling."""
        sensors = self._storage.list_sensors()
        states = {s['id']: s.get('updated_at') for s in sensors}
        return jsonify({'success': True, 'sensor_states': states})

    def api_device_rename(self, device_id: str):
        """Rename a device (called by core pairing manager and UI)."""
        payload = request.get_json(silent=True) or {}
        name = payload.get('name', '').strip()
        if not name:
            return jsonify({'success': False, 'error': 'Name is required'}), 400

        sensor = self._find_sensor_by_id(device_id)
        if sensor is None:
            return jsonify({'success': False, 'error': 'Device not found'}), 404

        updated = self._storage.update_sensor(device_id, name=name)
        return jsonify({'success': True, 'device': updated})

    def api_device_configure(self, device_id: str):
        """Configure a device (called by core and UI)."""
        payload = request.get_json(silent=True) or {}
        if not isinstance(payload, dict):
            return jsonify({'success': False, 'error': 'Invalid payload'}), 400

        sensor = self._find_sensor_by_id(device_id)
        if sensor is None:
            return jsonify({'success': False, 'error': 'Device not found'}), 404

        cfg, error = self._build_device_config(sensor, payload)
        if error:
            return jsonify({'success': False, 'error': error}), 400

        updated = self._storage.update_sensor(device_id, config=cfg, configured=True, config_pending=1)
        self._push_config_to_device_async(sensor['mac_address'], updated['config'])
        return jsonify({'success': True, 'device': updated})

    def api_device_delete(self, device_id: str):
        """Delete a device (called by core and UI)."""
        sensor = self._find_sensor_by_id(device_id)
        if sensor is None:
            return jsonify({'success': False, 'error': 'Device not found'}), 404

        try:
            sensor_mac = str(sensor.get('mac_address') or '').upper()
            self._storage.delete_sensor(device_id)

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

    def _request_sensor_hello(self, host, sensor_mac: str, sender_ip: str | None) -> None:
        if not host or not sensor_mac or not sender_ip:
            return

        try:
            network_server = host.require('network_server')
        except Exception:
            return

        try:
            network_server.send_pair_confirm(sensor_mac, sender_ip)
        except Exception as exc:
            self._log(f'Failed to request HELLO from {sensor_mac}: {exc}')

    def encode_temp_config(self, network_server, target_mac: str, config: dict, msg_id: int = 0) -> bytes:
        """Encode a temperature device configuration packet."""
        pkt_type = PKT_TEMP_CONFIG
        src_mac = b'\x00' * 6
        tgt_mac = binascii.unhexlify(target_mac.replace(":", ""))

        brightness = config.get('brightness', {})
        day_start = brightness.get('day_start', '07:00')
        try:
            day_h, day_m = map(int, day_start.split(':'))
        except Exception:
            day_h, day_m = 7, 0
        day_brightness = int(brightness.get('day', 100))

        night_start = brightness.get('night_start', '21:00')
        try:
            night_h, night_m = map(int, night_start.split(':'))
        except Exception:
            night_h, night_m = 21, 0
        night_brightness = int(brightness.get('night', 25))

        primary_name = config.get('primary_short_name', '') or ''
        primary_name_bytes = primary_name[:5].encode('utf-8')
        primary_name_bytes = primary_name_bytes + b'\x00' * (5 - len(primary_name_bytes))

        secondary = config.get('secondary', {}) or {}
        sec_enabled = 1 if secondary.get('enabled', False) else 0
        sec_name = secondary.get('short_name', '') or ''
        sec_name_bytes = sec_name[:5].encode('utf-8')
        sec_name_bytes = sec_name_bytes + b'\x00' * (5 - len(sec_name_bytes))

        sec_mac_str = '00:00:00:00:00:00'
        if sec_enabled:
            sec_sensor_id = secondary.get('sensor_id')
            if sec_sensor_id:
                sec_sensor = self._find_sensor_by_id(sec_sensor_id)
                if sec_sensor:
                    sec_mac_str = sec_sensor.get('mac_address', '00:00:00:00:00:00')

        sec_mac_bytes = binascii.unhexlify(sec_mac_str.replace(':', ''))

        report_interval = int(config.get('report_interval_seconds', DEFAULT_REPORT_INTERVAL))

        # Pack everything into payload
        payload_data = struct.pack(
            "<BBBBBB5sB5s6sI",
            day_h, day_m, day_brightness,
            night_h, night_m, night_brightness,
            primary_name_bytes,
            sec_enabled, sec_name_bytes, sec_mac_bytes,
            report_interval,
        )

        # Pad to 185 bytes
        payload = payload_data + b'\x00' * (185 - len(payload_data))
        signature = network_server.encoder._calculate_hash(payload)

        # final packet
        packet = struct.pack("<BI", pkt_type, signature) + src_mac + tgt_mac + struct.pack("<B", msg_id) + payload
        return packet

    def _push_config_to_device_async(self, sensor_mac: str, config: dict):
        """Trigger an asynchronous attempt to send configuration to the device."""
        import threading
        threading.Thread(
            target=self._push_config_to_device,
            args=(sensor_mac, config),
            daemon=True,
            name=f"PushConfig-{sensor_mac.replace(':', '')}"
        ).start()

    def _push_config_to_device(self, sensor_mac: str, config: dict) -> bool:
        """Send the configuration packet to the device. Returns True if successfully delivered."""
        host = self.context
        if not host:
            return False

        try:
            network_server = host.require('network_server')
        except Exception:
            return False

        try:
            msg_id = network_server.get_message_id()
            packet = self.encode_temp_config(network_server, sensor_mac, config, msg_id)
            sensor = self._find_sensor_by_id(sensor_mac)
            last_gateway = sensor.get('last_seen_gateway') if sensor else None
            success = network_server.send_raw_packet_to_device(sensor_mac, packet, wait_for_delivery=True, msg_id=msg_id, gateway_preference=last_gateway)
            if success:
                self._storage.update_sensor(sensor_mac, config_pending=0)
                self._forward_secondary_reading_on_startup(sensor_mac, host)
                self._log(f"Successfully synced config to temperature device {sensor_mac}")
                return True
            else:
                self._log(f"Failed to sync config to temperature device {sensor_mac} (device might be offline/asleep)")
                return False
        except Exception as exc:
            self._log(f"Exception during config sync to {sensor_mac}: {exc}")
            return False

    def _check_and_send_pending_config(self, sensor_mac: str):
        """If there is a pending configuration for this sensor, send it in a separate thread."""
        sensor = self._find_sensor_by_id(sensor_mac)
        if sensor and sensor.get('config_pending'):
            self._push_config_to_device_async(sensor_mac, sensor['config'])

    def _forward_secondary_reading_on_startup(self, sensor_mac: str, host):
        sensor = self._find_sensor_by_id(sensor_mac)
        if not sensor:
            return

        config = sensor.get('config') or {}
        secondary_config = config.get('secondary') or {}
        if not secondary_config.get('enabled'):
            return

        secondary_sensor_id = secondary_config.get('sensor_id')
        if not secondary_sensor_id:
            return

        secondary_sensor = self._find_sensor_by_id(secondary_sensor_id)
        if not secondary_sensor:
            return

        last_seen_gateway = sensor.get('last_seen_gateway')
        if not last_seen_gateway:
            return

        recent_readings = self._storage.list_readings(sensor_id=secondary_sensor_id, limit=1)
        if recent_readings:
            last_reading = recent_readings[0]
            timestamp_str = last_reading.get('timestamp')
            if timestamp_str:
                try:
                    timestamp = datetime.fromisoformat(timestamp_str)
                    age_seconds = (datetime.now(timezone.utc) - timestamp).total_seconds()
                    if age_seconds < 15 * 60:  # less than 15 minutes old
                        self._forward_secondary_reading_if_needed(secondary_sensor['mac_address'], last_reading, host)
                except Exception:
                    pass

    def _forward_secondary_reading_if_needed(self, secondary_mac: str, reading: dict, host):
        if not host:
            return

        try:
            network_server = host.require('network_server')
        except Exception:
            return

        secondary_id = secondary_mac.replace(':', '').lower()
        sensors = self._storage.list_sensors()
        for sensor in sensors:
            config = sensor.get('config') or {}
            sec_config = config.get('secondary') or {}
            if sec_config.get('enabled') and sec_config.get('sensor_id') == secondary_id:
                self._send_secondary_reading_to_device(network_server, sensor['mac_address'], reading)

    def _send_secondary_reading_to_device(self, network_server, target_mac: str, reading: dict):
        try:
            pkt_type = PKT_TEMP_SECONDARY
            src_mac = b'\x00' * 6
            tgt_mac = binascii.unhexlify(target_mac.replace(":", ""))

            temp_c = reading.get('temperature_c', 0.0)
            temp_x10 = int(round(temp_c * 10))

            humidity_pct = reading.get('humidity_pct')
            if humidity_pct is None:
                humidity_x10 = 0xFFFF
            else:
                humidity_x10 = int(round(humidity_pct * 10))

            battery_percentage = reading.get('battery_percent', 0)
            if not battery_percentage or battery_percentage < 0:
                battery_percentage = 255

            payload_data = struct.pack("<hHB", temp_x10, humidity_x10, battery_percentage)
            payload = payload_data + b'\x00' * (185 - len(payload_data))
            signature = network_server.encoder._calculate_hash(payload)

            msg_id = network_server.get_message_id()
            packet = struct.pack("<BI", pkt_type, signature) + src_mac + tgt_mac + struct.pack("<B", msg_id) + payload

            sensor = self._find_sensor_by_id(target_mac)
            last_gateway = sensor.get('last_seen_gateway') if sensor else None

            network_server.send_raw_packet_to_device(target_mac, packet, wait_for_delivery=True, msg_id=msg_id, gateway_preference=last_gateway)
            self._log(f"Forwarded secondary reading from {reading['sensor_mac']} to {target_mac}")
        except Exception as exc:
            self._log(f"Failed to forward secondary reading to {target_mac}: {exc}")

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
