"""SQLite-backed persistence for temperature tracking plugin data."""
from __future__ import annotations

import json
import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def default_device_config() -> dict[str, Any]:
    return {
        'report_interval_seconds': 600,
        'brightness': {
            'day_start': '07:00',
            'day': 100,
            'night_start': '21:00',
            'night': 25,
        },
        'primary_short_name': '',
        'secondary': {
            'enabled': False,
            'sensor_id': None,
            'short_name': '',
        },
    }


def _default_sensor(sensor_mac: str) -> dict[str, Any]:
    sensor_id = sensor_mac.replace(':', '').lower()
    now = _now_iso()
    return {
        'id': sensor_id,
        'name': f'Temp Sensor {sensor_mac[-8:]}',
        'mac_address': sensor_mac.upper(),
        'state': 'unknown',
        'configured': False,
        'config': default_device_config(),
        'config_pending': 0,
        'device_type': 0,
        'last_seen': None,
        'last_seen_gateway': None,
        'rssi': None,
        'version': '0.0.0',
        'platform': None,
        'temperature_c': None,
        'humidity_pct': None,
        'battery_mv': None,
        'battery_percent': None,
        'battery_last_updated': None,
        'flags': None,
        'sender_ip': None,
        'created_at': now,
        'updated_at': now,
    }


class TemperatureStorage:
    """Small SQLite store for temperature sensors and readings."""

    def __init__(self, db_path: Optional[str | Path] = None):
        if db_path is None:
            db_path = Path(__file__).resolve().parents[2] / 'data' / 'temperature_tracking.sqlite3'
        self.db_path = Path(db_path)
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._initialize()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        conn.execute('PRAGMA journal_mode=WAL')
        conn.execute('PRAGMA synchronous=NORMAL')
        conn.execute('PRAGMA foreign_keys=ON')
        return conn

    def _initialize(self) -> None:
        with self._lock, self._connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS sensors (
                    id TEXT PRIMARY KEY,
                    mac_address TEXT NOT NULL UNIQUE,
                    name TEXT NOT NULL,
                    state TEXT NOT NULL,
                    configured INTEGER NOT NULL DEFAULT 0,
                    config_json TEXT NOT NULL DEFAULT '{}',
                    config_pending INTEGER NOT NULL DEFAULT 0,
                    device_type INTEGER NOT NULL DEFAULT 0,
                    last_seen TEXT,
                    last_seen_gateway TEXT,
                    rssi INTEGER,
                    version TEXT,
                    platform TEXT,
                    temperature_c REAL,
                    humidity_pct REAL,
                    battery_mv INTEGER,
                    battery_percent INTEGER,
                    battery_last_updated TEXT,
                    flags INTEGER,
                    sender_ip TEXT,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS readings (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    sensor_mac TEXT NOT NULL,
                    temperature_c REAL NOT NULL,
                    humidity_pct REAL,
                    battery_mv INTEGER,
                    battery_percent INTEGER,
                    flags INTEGER,
                    timestamp TEXT NOT NULL,
                    sender_ip TEXT,
                    gateway_radio_mac TEXT,
                    FOREIGN KEY(sensor_mac) REFERENCES sensors(mac_address) ON DELETE CASCADE
                );

                CREATE INDEX IF NOT EXISTS idx_temperature_readings_sensor_mac
                    ON readings(sensor_mac);

                CREATE INDEX IF NOT EXISTS idx_temperature_readings_timestamp
                    ON readings(timestamp DESC);
                """
            )
            try:
                conn.execute("SELECT config_pending FROM sensors LIMIT 1")
            except sqlite3.OperationalError:
                conn.execute("ALTER TABLE sensors ADD COLUMN config_pending INTEGER NOT NULL DEFAULT 0")
                conn.commit()

            try:
                conn.execute("SELECT device_type FROM sensors LIMIT 1")
            except sqlite3.OperationalError:
                conn.execute("ALTER TABLE sensors ADD COLUMN device_type INTEGER NOT NULL DEFAULT 0")
                conn.commit()

    @staticmethod
    def _normalize_mac(sensor_mac: str) -> str:
        return str(sensor_mac or '').upper().strip()

    @staticmethod
    def _sensor_matches(row: sqlite3.Row, sensor_id: str) -> bool:
        normalized_id = str(sensor_id or '').strip().lower()
        if not normalized_id:
            return False
        row_id = str(row['id'] or '').lower()
        row_mac = str(row['mac_address'] or '').lower()
        row_mac_compact = row_mac.replace(':', '')
        return normalized_id in {row_id, row_mac, row_mac_compact}

    @staticmethod
    def _row_to_sensor(row: sqlite3.Row) -> dict[str, Any]:
        config = {}
        try:
            config = json.loads(row['config_json'] or '{}')
        except json.JSONDecodeError:
            config = {}

        return {
            'id': row['id'],
            'name': row['name'],
            'mac_address': row['mac_address'],
            'state': row['state'],
            'configured': bool(row['configured']),
            'config': config,
            'config_pending': int(row['config_pending'] or 0),
            'device_type': int(row['device_type'] or 0),
            'last_seen': row['last_seen'],
            'last_seen_gateway': row['last_seen_gateway'],
            'rssi': row['rssi'],
            'version': row['version'],
            'platform': row['platform'],
            'temperature_c': row['temperature_c'],
            'humidity_pct': row['humidity_pct'],
            'battery_mv': row['battery_mv'],
            'battery_percent': row['battery_percent'],
            'battery_last_updated': row['battery_last_updated'],
            'flags': row['flags'],
            'sender_ip': row['sender_ip'],
            'created_at': row['created_at'],
            'updated_at': row['updated_at'],
        }

    @staticmethod
    def _merge_config(current_config: dict[str, Any], updates: dict[str, Any]) -> dict[str, Any]:
        merged = dict(current_config or {})
        merged.update({key: value for key, value in updates.items() if value is not None})
        return merged

    def list_sensors(self) -> list[dict[str, Any]]:
        with self._lock, self._connect() as conn:
            rows = conn.execute(
                'SELECT * FROM sensors ORDER BY created_at ASC'
            ).fetchall()
            return [self._row_to_sensor(row) for row in rows]

    def get_sensor(self, sensor_id: str) -> dict[str, Any] | None:
        normalized_id = str(sensor_id or '').strip().lower()
        if not normalized_id:
            return None

        with self._lock, self._connect() as conn:
            rows = conn.execute('SELECT * FROM sensors ORDER BY updated_at DESC').fetchall()
            for row in rows:
                if self._sensor_matches(row, normalized_id):
                    return self._row_to_sensor(row)
        return None

    def upsert_sensor(self, sensor_mac: str, **updates: Any) -> dict[str, Any]:
        mac_address = self._normalize_mac(sensor_mac)
        if not mac_address:
            raise ValueError('sensor_mac is required')

        now = _now_iso()
        with self._lock, self._connect() as conn:
            row = conn.execute('SELECT * FROM sensors WHERE mac_address = ?', (mac_address,)).fetchone()
            sensor = self._row_to_sensor(row) if row else _default_sensor(mac_address)

            config_updates = updates.pop('config', None)
            if isinstance(config_updates, dict):
                if config_updates.pop('__replace__', False):
                    sensor['config'] = dict(config_updates)
                else:
                    sensor['config'] = self._merge_config(sensor.get('config', {}), config_updates)

            for key, value in updates.items():
                if value is not None:
                    sensor[key] = value

            sensor['mac_address'] = mac_address
            sensor['id'] = str(sensor.get('id') or mac_address.replace(':', '').lower())
            sensor['name'] = str(sensor.get('name') or f'Temp Sensor {mac_address[-8:]}')
            sensor['state'] = str(sensor.get('state') or 'unknown')
            sensor['configured'] = bool(sensor.get('configured', False))
            sensor['config'] = sensor.get('config') if isinstance(sensor.get('config'), dict) else default_device_config()
            sensor['created_at'] = sensor.get('created_at') or now
            sensor['updated_at'] = now

            conn.execute(
                """
                INSERT INTO sensors (
                    id, mac_address, name, state, configured, config_json, config_pending, device_type,
                    last_seen, last_seen_gateway, rssi, version, platform,
                    temperature_c, humidity_pct, battery_mv, battery_percent,
                    battery_last_updated, flags, sender_ip, created_at, updated_at
                ) VALUES (
                    :id, :mac_address, :name, :state, :configured, :config_json, :config_pending, :device_type,
                    :last_seen, :last_seen_gateway, :rssi, :version, :platform,
                    :temperature_c, :humidity_pct, :battery_mv, :battery_percent,
                    :battery_last_updated, :flags, :sender_ip, :created_at, :updated_at
                )
                ON CONFLICT(mac_address) DO UPDATE SET
                    id = excluded.id,
                    name = excluded.name,
                    state = excluded.state,
                    configured = excluded.configured,
                    config_json = excluded.config_json,
                    config_pending = excluded.config_pending,
                    device_type = excluded.device_type,
                    last_seen = excluded.last_seen,
                    last_seen_gateway = excluded.last_seen_gateway,
                    rssi = excluded.rssi,
                    version = excluded.version,
                    platform = excluded.platform,
                    temperature_c = excluded.temperature_c,
                    humidity_pct = excluded.humidity_pct,
                    battery_mv = excluded.battery_mv,
                    battery_percent = excluded.battery_percent,
                    battery_last_updated = excluded.battery_last_updated,
                    flags = excluded.flags,
                    sender_ip = excluded.sender_ip,
                    updated_at = excluded.updated_at
                """,
                {
                    **sensor,
                    'configured': 1 if sensor.get('configured') else 0,
                    'config_pending': int(sensor.get('config_pending', 0)),
                    'config_json': json.dumps(sensor.get('config', {})),
                },
            )
            conn.commit()

        return self.get_sensor(mac_address) or sensor

    def update_sensor(self, sensor_id: str, **updates: Any) -> dict[str, Any] | None:
        sensor = self.get_sensor(sensor_id)
        if sensor is None:
            return None
        return self.upsert_sensor(sensor['mac_address'], **updates)

    def delete_sensor(self, sensor_id: str) -> bool:
        sensor = self.get_sensor(sensor_id)
        if sensor is None:
            return False

        with self._lock, self._connect() as conn:
            conn.execute('DELETE FROM readings WHERE sensor_mac = ?', (sensor['mac_address'],))
            conn.execute('DELETE FROM sensors WHERE mac_address = ?', (sensor['mac_address'],))
            conn.commit()
        return True

    def add_reading(
        self,
        sensor_mac: str,
        *,
        temperature_c: float,
        humidity_pct: float | None = None,
        battery_mv: int | None = None,
        battery_percent: int | None = None,
        flags: int | None = None,
        sender_ip: str | None = None,
        gateway_radio_mac: str | None = None,
        timestamp: str | None = None,
    ) -> dict[str, Any]:
        mac_address = self._normalize_mac(sensor_mac)
        if not mac_address:
            raise ValueError('sensor_mac is required')

        reading_timestamp = timestamp or _now_iso()
        with self._lock, self._connect() as conn:
            conn.execute(
                """
                INSERT INTO readings (
                    sensor_mac, temperature_c, humidity_pct, battery_mv, battery_percent,
                    flags, timestamp, sender_ip, gateway_radio_mac
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    mac_address,
                    temperature_c,
                    humidity_pct,
                    battery_mv,
                    battery_percent,
                    flags,
                    reading_timestamp,
                    sender_ip,
                    gateway_radio_mac,
                ),
            )
            conn.commit()

        return {
            'sensor_mac': mac_address,
            'temperature_c': temperature_c,
            'humidity_pct': humidity_pct,
            'battery_mv': battery_mv,
            'battery_percent': battery_percent,
            'flags': flags,
            'timestamp': reading_timestamp,
            'sender_ip': sender_ip,
            'gateway_radio_mac': gateway_radio_mac,
        }

    def list_readings(
        self,
        limit: int = 100,
        sensor_id: str | None = None,
        start: str | None = None,
        end: str | None = None,
    ) -> list[dict[str, Any]]:
        safe_limit = max(1, min(int(limit or 100), 5000))
        query = 'SELECT * FROM readings'
        params: list[Any] = []
        conditions: list[str] = []

        if sensor_id:
            sensor = self.get_sensor(sensor_id)
            if sensor is None:
                return []
            conditions.append('sensor_mac = ?')
            params.append(sensor['mac_address'])

        if start:
            conditions.append('datetime(timestamp) >= datetime(?)')
            params.append(start)
        if end:
            conditions.append('datetime(timestamp) <= datetime(?)')
            params.append(end)

        if conditions:
            query += ' WHERE ' + ' AND '.join(conditions)

        query += ' ORDER BY datetime(timestamp) DESC, id DESC LIMIT ?'
        params.append(safe_limit)

        with self._lock, self._connect() as conn:
            rows = conn.execute(query, params).fetchall()
            return [
                {
                    'id': row['id'],
                    'sensor_mac': row['sensor_mac'],
                    'temperature_c': row['temperature_c'],
                    'humidity_pct': row['humidity_pct'],
                    'battery_mv': row['battery_mv'],
                    'battery_percent': row['battery_percent'],
                    'flags': row['flags'],
                    'timestamp': row['timestamp'],
                    'sender_ip': row['sender_ip'],
                    'gateway_radio_mac': row['gateway_radio_mac'],
                }
                for row in rows
            ]

    def get_readings_aggregate(
        self,
        sensor_id: str,
        start: str | None = None,
        end: str | None = None,
        bucket: str = 'hour',
    ) -> list[dict[str, Any]]:
        sensor = self.get_sensor(sensor_id)
        if sensor is None:
            return []

        if bucket == 'day':
            fmt = '%Y-%m-%d'
        else:
            fmt = '%Y-%m-%dT%H:00:00'

        query = f"""
            SELECT
                strftime('{fmt}', datetime(timestamp, 'localtime')) AS bucket,
                AVG(temperature_c) AS avg_temp,
                MIN(temperature_c) AS min_temp,
                MAX(temperature_c) AS max_temp,
                AVG(humidity_pct) AS avg_humidity,
                MIN(humidity_pct) AS min_humidity,
                MAX(humidity_pct) AS max_humidity,
                AVG(battery_percent) AS avg_battery,
                COUNT(*) AS count
            FROM readings
            WHERE sensor_mac = ?
        """
        params: list[Any] = [sensor['mac_address']]

        if start:
            query += ' AND datetime(timestamp) >= datetime(?)'
            params.append(start)
        if end:
            query += ' AND datetime(timestamp) <= datetime(?)'
            params.append(end)

        query += ' GROUP BY bucket ORDER BY bucket ASC'

        with self._lock, self._connect() as conn:
            rows = conn.execute(query, params).fetchall()
            return [
                {
                    'bucket': row['bucket'],
                    'avg_temp': row['avg_temp'],
                    'min_temp': row['min_temp'],
                    'max_temp': row['max_temp'],
                    'avg_humidity': row['avg_humidity'],
                    'min_humidity': row['min_humidity'],
                    'max_humidity': row['max_humidity'],
                    'avg_battery': row['avg_battery'],
                    'count': row['count'],
                }
                for row in rows
            ]