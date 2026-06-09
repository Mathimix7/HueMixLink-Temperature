let sensors = [];
let currentSensorId = null;
let lastSensorStates = {};

function formatDate(value) {
    if (!value) return 'Never';
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return 'Unknown';
    return date.toLocaleString();
}

function getSensorStatus(sensor) {
    if (sensor.config_pending) {
        return { color: '#EAB308', label: 'Pending config — not yet received by device' };
    }
    if (!sensor.last_seen) {
        return { color: '#EF4444', label: 'Offline — no data received yet' };
    }
    const intervalMs = ((sensor.config && sensor.config.report_interval_seconds) || 600) * 1000;
    const lastSeen = new Date(sensor.last_seen).getTime();
    if (Date.now() - lastSeen > intervalMs * 2.5) {
        return { color: '#EF4444', label: 'Offline — no data within expected reporting interval' };
    }
    return { color: '#22C55E', label: 'Online — reporting normally' };
}

function getBatteryDisplay(sensor) {
    if (sensor.battery_percent === undefined || sensor.battery_percent === null) {
        return '<span class="text-sm text-gray-400 flex items-center space-x-2"><i class="fas fa-battery-empty text-gray-400"></i><span>N/A</span></span>';
    }

    let iconClass = 'fa-battery-half';
    let colorClass = 'text-yellow-600';

    if (sensor.battery_percent >= 60) {
        iconClass = 'fa-battery-full';
        colorClass = 'text-green-600';
    } else if (sensor.battery_percent < 20) {
        iconClass = 'fa-battery-quarter';
        colorClass = 'text-red-600';
    }

    const lastUpdated = formatDate(sensor.battery_last_updated);
    return `
        <div class="flex items-center space-x-2 battery-tooltip">
            <i class="fas ${iconClass} ${colorClass}"></i>
            <span class="text-sm font-medium ${colorClass}">${sensor.battery_percent}%</span>
            <div class="tooltip-content">
                <div><strong>Voltage:</strong> ${sensor.battery_mv || 'N/A'} mV</div>
                <div><strong>Updated:</strong> ${lastUpdated}</div>
            </div>
        </div>
    `;
}

function pollSensorStates() {
    fetch('/plugins/temperature/api/sensor_states')
        .then(response => response.json())
        .then(data => {
            if (data.success && data.sensor_states) {
                const states = data.sensor_states;
                for (const [id, updatedAt] of Object.entries(states)) {
                    if (updatedAt && lastSensorStates[id] && lastSensorStates[id] !== updatedAt) {
                        flashSensorRow(id);
                        fetch('/plugins/temperature/sensors')
                            .then(resp => resp.json())
                            .then(sensorData => {
                                const updated = (Array.isArray(sensorData) ? sensorData : []).find(s => s.id === id);
                                if (updated) {
                                    const idx = sensors.findIndex(s => s.id === id);
                                    if (idx !== -1) sensors[idx] = updated;
                                    else sensors.push(updated);
                                    updateDeviceRow(updated);
                                }
                            })
                            .catch(() => loadSensors());
                    }
                    if (!sensors.find(s => s.id === id)) {
                        loadSensors();
                    }
                    lastSensorStates[id] = updatedAt;
                }
            }
        })
        .catch(() => {});
}

function flashSensorRow(deviceId) {
    const tbody = document.getElementById('devices-table-body');
    if (!tbody) return;
    const row = Array.from(tbody.children).find(r => r.dataset && r.dataset.deviceId === deviceId);
    if (row) {
        row.classList.remove('flash-temperature');
        void row.offsetWidth;
        row.classList.add('flash-temperature');
        setTimeout(() => row.classList.remove('flash-temperature'), 700);
    }
}

function updateDeviceRow(device) {
    const tbody = document.getElementById('devices-table-body');
    if (!tbody) return;
    const row = Array.from(tbody.children).find(r => r.dataset && r.dataset.deviceId === device.id);
    if (!row) return;

    const nameCell = row.children[0];
    if (nameCell) {
        const isBattery = device.device_type === 1;
        const badge = isBattery
            ? '<span class="ml-2 px-2 py-0.5 inline-flex text-xs leading-5 font-semibold rounded-full bg-green-100 text-green-800">Battery</span>'
            : '<span class="ml-2 px-2 py-0.5 inline-flex text-xs leading-5 font-semibold rounded-full bg-blue-100 text-blue-800">OLED Screen</span>';
        const status = getSensorStatus(device);
        nameCell.innerHTML = `<div class="flex items-center"><span class="inline-block w-2.5 h-2.5 rounded-full mr-2 flex-shrink-0" style="background-color:${status.color}" title="${status.label}"></span><span class="text-sm font-medium text-gray-900">${device.name || ''}</span>${badge}</div>`;
    }

    const tempCell = row.children[2];
    if (tempCell) {
        tempCell.innerHTML = `<span class="text-sm text-gray-900">${device.temperature_c !== undefined && device.temperature_c !== null ? device.temperature_c.toFixed(1) + '°C' : '—'}</span>`;
    }

    const humidCell = row.children[3];
    if (humidCell) {
        humidCell.innerHTML = `<span class="text-sm text-gray-700">${device.humidity_pct !== undefined && device.humidity_pct !== null ? device.humidity_pct.toFixed(1) + '%' : '—'}</span>`;
    }

    const batteryCell = row.children[4];
    if (batteryCell) {
        batteryCell.innerHTML = getBatteryDisplay(device);
    }
}

window.addEventListener('DOMContentLoaded', function() {
    loadSensors();
    setInterval(pollSensorStates, 2000);
});

function refreshSensors() {
    const refreshIcon = document.getElementById('refresh-icon');
    if (refreshIcon) {
        refreshIcon.classList.add('rotate-360');
        setTimeout(() => refreshIcon.classList.remove('rotate-360'), 500);
    }
    loadSensors();
}

function loadSensors() {
    const tbody = document.getElementById('devices-table-body');
    if (!tbody) return;
    tbody.innerHTML = '';

    // Show skeleton rows
    for (let i = 0; i < 3; i++) {
        const row = document.createElement('tr');
        row.className = 'animate-pulse';
        row.innerHTML = `
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-40"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-36"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-28"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-20"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-24"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-20"></div>
            </td>
        `;
        tbody.appendChild(row);
    }

    fetch('/plugins/temperature/sensors')
        .then(resp => resp.json())
        .then(data => {
            sensors = Array.isArray(data) ? data : [];
            renderSensors();
        })
        .catch(err => {
            console.error('Error loading sensors:', err);
            showToast('Error', 'Failed to load temperature sensors', 'error');
            const tbody = document.getElementById('devices-table-body');
            if (tbody) tbody.innerHTML = `<tr><td colspan="6" class="px-6 py-12 text-center text-gray-500"><p class="text-lg">Unable to load sensors</p></td></tr>`;
        });
}

function renderSensors() {
    const tbody = document.getElementById('devices-table-body');
    if (!tbody) return;
    tbody.innerHTML = '';

    if (sensors.length === 0) {
        tbody.innerHTML = `
            <tr>
                <td colspan="6" class="px-6 py-12 text-center text-gray-500">
                    <p class="text-lg">No temperature sensors found</p>
                    <p class="text-sm mt-1">Sensors will appear here once paired.</p>
                </td>
            </tr>
        `;
        return;
    }

    sensors.forEach(sensor => {
        const row = document.createElement('tr');
        row.className = 'hover:bg-gray-50';
        row.dataset.deviceId = sensor.id;

        const nameCell = document.createElement('td');
        nameCell.className = 'px-6 py-4 whitespace-nowrap';
        const isBattery = sensor.device_type === 1;
        const iconColor = isBattery ? 'text-green-500' : 'text-blue-500';
        const badge = isBattery ? '<span class="ml-2 px-2 py-0.5 inline-flex text-xs leading-5 font-semibold rounded-full bg-green-100 text-green-800">Battery</span>' : '<span class="ml-2 px-2 py-0.5 inline-flex text-xs leading-5 font-semibold rounded-full bg-blue-100 text-blue-800">OLED Screen</span>';
        const status = getSensorStatus(sensor);
        nameCell.innerHTML = `<div class="flex items-center"><span class="inline-block w-2.5 h-2.5 rounded-full mr-2 flex-shrink-0" style="background-color:${status.color}" title="${status.label}"></span><span class="text-sm font-medium text-gray-900">${sensor.name || ''}</span>${badge}</div>`;

        const macCell = document.createElement('td');
        macCell.className = 'px-6 py-4 whitespace-nowrap';
        macCell.innerHTML = `<span class="text-sm text-gray-500 font-mono">${sensor.mac_address || ''}</span>`;

        const tempCell = document.createElement('td');
        tempCell.className = 'px-6 py-4 whitespace-nowrap';
        tempCell.innerHTML = `<span class="text-sm text-gray-900">${sensor.temperature_c !== undefined && sensor.temperature_c !== null ? sensor.temperature_c.toFixed(1) + '°C' : '—'}</span>`;

        const humidCell = document.createElement('td');
        humidCell.className = 'px-6 py-4 whitespace-nowrap';
        humidCell.innerHTML = `<span class="text-sm text-gray-700">${sensor.humidity_pct !== undefined && sensor.humidity_pct !== null ? sensor.humidity_pct.toFixed(1) + '%' : '—'}</span>`;

        const batteryCell = document.createElement('td');
        batteryCell.className = 'px-6 py-4 whitespace-nowrap';
        batteryCell.innerHTML = getBatteryDisplay(sensor);

        const actionsCell = document.createElement('td');
        actionsCell.className = 'px-6 py-4 whitespace-nowrap text-sm font-medium space-x-2';

        const renameBtn = document.createElement('button');
        renameBtn.className = 'text-blue-600 hover:text-blue-900 inline-flex items-center';
        renameBtn.innerHTML = '<i class="fas fa-edit mr-1"></i> Rename';
        renameBtn.addEventListener('click', () => openRenameModal(sensor.id, sensor.name));

        const configBtn = document.createElement('button');
        configBtn.className = 'text-green-600 hover:text-green-900 inline-flex items-center';
        configBtn.innerHTML = '<i class="fas fa-cog mr-1"></i> Configure';
        configBtn.addEventListener('click', () => openConfigModal(sensor.id, sensor.name, sensor.config || {}));

        const deleteBtn = document.createElement('button');
        deleteBtn.className = 'text-red-600 hover:text-red-900 inline-flex items-center';
        deleteBtn.innerHTML = '<i class="fas fa-trash mr-1"></i> Delete';
        deleteBtn.addEventListener('click', () => deleteSensor(sensor.id, sensor.name));

        actionsCell.appendChild(renameBtn);
        actionsCell.appendChild(configBtn);
        actionsCell.appendChild(deleteBtn);

        row.appendChild(nameCell);
        row.appendChild(macCell);
        row.appendChild(tempCell);
        row.appendChild(humidCell);
        row.appendChild(batteryCell);
        row.appendChild(actionsCell);

        tbody.appendChild(row);
    });
}

// Rename modal handlers
function openRenameModal(sensorId, sensorName) {
    currentSensorId = sensorId;
    document.getElementById('device-name').value = sensorName || '';
    document.getElementById('rename-modal').classList.remove('hidden');
}

function closeRenameModal() {
    document.getElementById('rename-modal').classList.add('hidden');
    currentSensorId = null;
}

function saveDeviceName() {
    const newName = document.getElementById('device-name').value.trim();
    if (!newName || !currentSensorId) return showToast('Validation Error', 'Please enter a name', 'warning');

    fetch(`/plugins/temperature/api/devices/${currentSensorId}/rename`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({name: newName})
    }).then(r => r.json()).then(data => {
        if (data.success) {
            showToast('Success', 'Sensor renamed', 'success');
            closeRenameModal();
            loadSensors();
        } else {
            showToast('Error', data.error || 'Rename failed', 'error');
        }
    }).catch(err => {
        console.error('Rename error', err);
        showToast('Error', 'Rename failed', 'error');
    });
}

// Config modal handlers
function openConfigModal(sensorId, sensorName, config) {
    currentSensorId = sensorId;
    const sensor = sensors.find(s => s.id === sensorId);
    const isBattery = sensor && sensor.device_type === 1;

    // Show/hide sections based on battery sensor type
    if (isBattery) {
        document.getElementById('config-brightness-schedule').classList.add('hidden');
        document.getElementById('config-primary-display').classList.add('hidden');
        document.getElementById('config-secondary-display').classList.add('hidden');
        document.getElementById('config-save-btn').classList.remove('hidden');
        document.getElementById('config-battery-sensor-note').classList.remove('hidden');
        document.getElementById('config-report-interval').classList.remove('hidden');
    } else {
        document.getElementById('config-brightness-schedule').classList.remove('hidden');
        document.getElementById('config-primary-display').classList.remove('hidden');
        document.getElementById('config-secondary-display').classList.remove('hidden');
        document.getElementById('config-save-btn').classList.remove('hidden');
        document.getElementById('config-battery-sensor-note').classList.add('hidden');
        document.getElementById('config-report-interval').classList.remove('hidden');
    }

    const brightness = config && config.brightness ? config.brightness : {};
    const secondary = config && config.secondary ? config.secondary : {};
    document.getElementById('config-device-name').textContent = sensorName || '';
    document.getElementById('config-day-start').value = brightness.day_start || '07:00';
    document.getElementById('config-day-brightness').value = brightness.day ?? 100;
    document.getElementById('config-night-start').value = brightness.night_start || '21:00';
    document.getElementById('config-night-brightness').value = brightness.night ?? 25;
    document.getElementById('config-primary-short-name').value = config && config.primary_short_name ? config.primary_short_name : '';
    document.getElementById('config-secondary-enabled').checked = !!secondary.enabled;
    document.getElementById('config-secondary-short-name').value = secondary.short_name || '';
    // Set report interval (stored in seconds, UI in minutes)
    const intervalSec = (config && config.report_interval_seconds) || 600;
    const intervalMin = Math.round(intervalSec / 60);
    const intervalSelect = document.getElementById('config-report-interval-minutes');
    if (intervalSelect) {
        if ([1, 2, 5, 10, 15, 30, 60, 120, 360].includes(intervalMin)) {
            intervalSelect.value = intervalMin;
        } else {
            intervalSelect.value = 10;
        }
    }
    populateSecondarySensorOptions(sensorId, secondary.sensor_id || '');
    updateSecondaryDisplayState();
    document.getElementById('config-modal').classList.remove('hidden');
}

function populateSecondarySensorOptions(currentSensorId, selectedSensorId) {
    const select = document.getElementById('config-secondary-sensor');
    if (!select) return;

    select.innerHTML = '';

    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = 'Choose a sensor';
    select.appendChild(placeholder);

    sensors.forEach(sensor => {
        if (!sensor || sensor.id === currentSensorId) return;
        const label = sensor.name ? `${sensor.name} (${sensor.mac_address || sensor.id || ''})` : (sensor.mac_address || sensor.id || 'Unknown sensor');
        const option = document.createElement('option');
        option.value = sensor.id;
        option.textContent = label;
        select.appendChild(option);
    });

    select.value = selectedSensorId || '';
    select.disabled = select.options.length <= 1;
}

function updateSecondaryDisplayState() {
    const enabled = document.getElementById('config-secondary-enabled').checked;
    const sensorSelect = document.getElementById('config-secondary-sensor');
    const shortNameInput = document.getElementById('config-secondary-short-name');

    if (sensorSelect) sensorSelect.disabled = !enabled || sensorSelect.options.length <= 1;
    if (shortNameInput) shortNameInput.disabled = !enabled;
}

function closeConfigModal() {
    document.getElementById('config-modal').classList.add('hidden');
    currentSensorId = null;
}

function saveConfig() {
    if (!currentSensorId) return;

    const primaryShortName = document.getElementById('config-primary-short-name').value.trim();
    const secondaryEnabled = document.getElementById('config-secondary-enabled').checked;
    const secondarySensorId = document.getElementById('config-secondary-sensor').value;
    const secondaryShortName = document.getElementById('config-secondary-short-name').value.trim();
    const dayBrightnessValue = document.getElementById('config-day-brightness').value;
    const nightBrightnessValue = document.getElementById('config-night-brightness').value;

    if (primaryShortName.length > 5) return showToast('Validation Error', 'Primary short name must be 5 characters or fewer', 'warning');
    if (secondaryShortName.length > 5) return showToast('Validation Error', 'Secondary short name must be 5 characters or fewer', 'warning');
    if (secondaryEnabled && !secondarySensorId) return showToast('Validation Error', 'Pick a secondary sensor', 'warning');

    const intervalSelect = document.getElementById('config-report-interval-minutes');
    const intervalMinutes = intervalSelect ? parseInt(intervalSelect.value, 10) : 10;

    const payload = {
        report_interval_seconds: intervalMinutes * 60,
        brightness: {
            day_start: document.getElementById('config-day-start').value,
            day: dayBrightnessValue === '' ? 100 : Number(dayBrightnessValue),
            night_start: document.getElementById('config-night-start').value,
            night: nightBrightnessValue === '' ? 25 : Number(nightBrightnessValue),
        },
        primary_short_name: primaryShortName,
        secondary: {
            enabled: secondaryEnabled,
            sensor_id: secondaryEnabled ? secondarySensorId : null,
            short_name: secondaryEnabled ? secondaryShortName : '',
        },
    };

    if (secondaryEnabled && secondarySensorId === currentSensorId) {
        return showToast('Validation Error', 'Secondary display must use a different sensor', 'warning');
    }

    fetch(`/plugins/temperature/api/devices/${currentSensorId}/configure`, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
    }).then(r => r.json()).then(data => {
        if (data.success) {
            showToast('Saved', 'Configuration updated', 'success');
            closeConfigModal();
            loadSensors();
        } else {
            showToast('Error', data.error || 'Save failed', 'error');
        }
    }).catch(err => {
        console.error('Config save error', err);
        showToast('Error', 'Save failed', 'error');
    });
}

document.addEventListener('change', function(event) {
    if (event.target && event.target.id === 'config-secondary-enabled') {
        updateSecondaryDisplayState();
    }
});

// Delete handlers
let pendingDeleteId = null;
function deleteSensor(sensorId, sensorName) {
    pendingDeleteId = sensorId;
    document.getElementById('delete-device-name').textContent = sensorName || '';
    document.getElementById('delete-modal').classList.remove('hidden');
}

function closeDeleteModal() {
    document.getElementById('delete-modal').classList.add('hidden');
    pendingDeleteId = null;
}

function confirmDelete() {
    if (!pendingDeleteId) return;
    fetch(`/plugins/temperature/api/devices/${pendingDeleteId}`, {method: 'DELETE'})
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                showToast('Deleted', 'Sensor removed', 'success');
                closeDeleteModal();
                loadSensors();
            } else {
                showToast('Error', data.error || 'Delete failed', 'error');
            }
        })
        .catch(err => {
            console.error('Delete error', err);
            showToast('Error', 'Delete failed', 'error');
        });
}
