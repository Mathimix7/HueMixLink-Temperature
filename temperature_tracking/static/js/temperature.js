let sensors = [];
let currentSensorId = null;

window.addEventListener('DOMContentLoaded', function() {
    loadSensors();
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
                <div class="h-4 bg-gray-200 rounded w-20"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-20"></div>
            </td>
            <td class="px-6 py-4 whitespace-nowrap">
                <div class="h-4 bg-gray-200 rounded w-24"></div>
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
            if (tbody) tbody.innerHTML = `<tr><td colspan="7" class="px-6 py-12 text-center text-gray-500"><p class="text-lg">Unable to load sensors</p></td></tr>`;
        });
}

function renderSensors() {
    const tbody = document.getElementById('devices-table-body');
    if (!tbody) return;
    tbody.innerHTML = '';

    if (sensors.length === 0) {
        tbody.innerHTML = `
            <tr>
                <td colspan="7" class="px-6 py-12 text-center text-gray-500">
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

        const nameCell = document.createElement('td');
        nameCell.className = 'px-6 py-4 whitespace-nowrap';
        nameCell.innerHTML = `<div class="flex items-center"><i class="fas fa-thermometer-half mr-2 text-blue-500"></i><span class="text-sm font-medium text-gray-900">${sensor.name || ''}</span></div>`;

        const macCell = document.createElement('td');
        macCell.className = 'px-6 py-4 whitespace-nowrap';
        macCell.innerHTML = `<span class="text-sm text-gray-500 font-mono">${sensor.mac_address || ''}</span>`;

        const tempCell = document.createElement('td');
        tempCell.className = 'px-6 py-4 whitespace-nowrap';
        tempCell.innerHTML = `<span class="text-sm font-medium text-gray-900">${sensor.temperature_c !== undefined && sensor.temperature_c !== null ? sensor.temperature_c.toFixed(1) + '°C' : 'N/A'}</span>`;

        const humidCell = document.createElement('td');
        humidCell.className = 'px-6 py-4 whitespace-nowrap';
        humidCell.innerHTML = `<span class="text-sm text-gray-700">${sensor.humidity_pct !== undefined && sensor.humidity_pct !== null ? sensor.humidity_pct.toFixed(1) + '%' : '—'}</span>`;

        const batteryCell = document.createElement('td');
        batteryCell.className = 'px-6 py-4 whitespace-nowrap';
        batteryCell.innerHTML = sensor.last_battery_mv ? `<span class="text-sm text-gray-700">${sensor.last_battery_mv} mV</span>` : `<span class="text-sm text-gray-400">N/A</span>`;

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
    document.getElementById('config-device-name').textContent = sensorName || '';
    document.getElementById('config-enabled').checked = !!(config && config.enabled);
    document.getElementById('config-min-temp').value = config && config.min_temperature_c != null ? config.min_temperature_c : '';
    document.getElementById('config-max-temp').value = config && config.max_temperature_c != null ? config.max_temperature_c : '';
    document.getElementById('config-modal').classList.remove('hidden');
}

function closeConfigModal() {
    document.getElementById('config-modal').classList.add('hidden');
    currentSensorId = null;
}

function saveConfig() {
    if (!currentSensorId) return;
    const payload = {
        enabled: document.getElementById('config-enabled').checked,
        min_temperature_c: document.getElementById('config-min-temp').value ? parseFloat(document.getElementById('config-min-temp').value) : null,
        max_temperature_c: document.getElementById('config-max-temp').value ? parseFloat(document.getElementById('config-max-temp').value) : null,
    };

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
