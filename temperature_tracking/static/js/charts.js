const COLOR_PALETTE = [
    '#EF4444', '#3B82F6', '#10B981', '#F59E0B',
    '#8B5CF6', '#EC4899', '#06B6D4', '#F97316',
    '#84CC16', '#14B8A6', '#D946EF', '#0EA5E9',
    '#22C55E', '#EAB308', '#A855F7', '#64748B',
];

const TIME_RANGES = {
    '6h':  { ms: 6 * 60 * 60 * 1000, bucket: 'raw',   label: '6 hours' },
    '24h': { ms: 24 * 60 * 60 * 1000, bucket: 'raw',   label: '24 hours' },
    '7d':  { ms: 7 * 24 * 60 * 60 * 1000, bucket: 'hour', label: '7 days' },
    '30d': { ms: 30 * 24 * 60 * 60 * 1000, bucket: 'day',  label: '30 days' },
    'all': { ms: null, bucket: 'day', label: 'All time' },
};

const sensors = Array.isArray(window.__SENSORS) ? window.__SENSORS : [];
let tempChart = null;
let humidityChart = null;
let activeRange = '24h';
let enabledSensors = new Set();

function getColor(index) {
    return COLOR_PALETTE[index % COLOR_PALETTE.length];
}

function getTimeRangeStart(rangeKey) {
    const range = TIME_RANGES[rangeKey];
    if (!range || !range.ms) return null;
    return new Date(Date.now() - range.ms).toISOString();
}

function getMaxGapMs(rangeKey) {
    if (rangeKey === '6h')  return 30 * 60 * 1000;
    if (rangeKey === '24h') return 2 * 60 * 60 * 1000;
    if (rangeKey === '7d')  return 6 * 60 * 60 * 1000;
    if (rangeKey === '30d') return 12 * 60 * 60 * 1000;
    return 24 * 60 * 60 * 1000;
}

function segmentConfig(rangeKey) {
    const maxGap = getMaxGapMs(rangeKey);
    return {
        borderColor: ctx => {
            const gap = ctx.p1.parsed.x - ctx.p0.parsed.x;
            return gap > maxGap ? 'transparent' : undefined;
        },
        borderWidth: ctx => {
            const gap = ctx.p1.parsed.x - ctx.p0.parsed.x;
            return gap > maxGap ? 0 : undefined;
        },
        borderDash: ctx => {
            const gap = ctx.p1.parsed.x - ctx.p0.parsed.x;
            return gap > maxGap ? [] : undefined;
        },
    };
}

function buildSensorCheckboxes() {
    const container = document.getElementById('sensor-checkboxes');
    if (!container) return;

    if (sensors.length === 0) {
        container.innerHTML = '<span class="text-sm text-gray-400 italic">No sensors available</span>';
        return;
    }

    sensors.forEach((sensor, index) => {
        const id = sensor.id || sensor.mac_address;
        const label = sensor.name || sensor.mac_address || `Sensor ${index + 1}`;
        const color = getColor(index);

        const wrapper = document.createElement('label');
        wrapper.className = 'inline-flex items-center gap-1.5 px-2.5 py-1 rounded-lg border border-gray-200 cursor-pointer hover:bg-gray-50 transition-colors text-sm sensor-checkbox';
        wrapper.dataset.sensorId = id;

        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.checked = true;
        cb.className = 'h-3.5 w-3.5 rounded border-gray-300 focus:ring-2 focus:ring-offset-0';
        cb.style.accentColor = color;
        cb.dataset.sensorId = id;
        cb.addEventListener('change', () => onSensorToggle(id));

        const dot = document.createElement('span');
        dot.className = 'w-2.5 h-2.5 rounded-full inline-block';
        dot.style.backgroundColor = color;

        const text = document.createElement('span');
        text.className = 'text-gray-700';
        text.textContent = label;

        wrapper.appendChild(cb);
        wrapper.appendChild(dot);
        wrapper.appendChild(text);
        container.appendChild(wrapper);

        enabledSensors.add(id);
    });
}

function onSensorToggle(sensorId) {
    if (enabledSensors.has(sensorId)) {
        enabledSensors.delete(sensorId);
    } else {
        enabledSensors.add(sensorId);
    }
    updateCharts();
}

function setupTimeRangeButtons() {
    const buttons = document.querySelectorAll('#time-range-buttons button');
    buttons.forEach(btn => {
        btn.addEventListener('click', () => {
            buttons.forEach(b => {
                b.classList.remove('bg-blue-600', 'text-white');
                b.classList.add('text-gray-600', 'hover:bg-gray-100');
            });
            btn.classList.remove('text-gray-600', 'hover:bg-gray-100');
            btn.classList.add('bg-blue-600', 'text-white');
            activeRange = btn.dataset.range;
            updateCharts();
        });
    });
}

async function fetchSensorData(sensor, start, bucket) {
    const sensorId = sensor.id || sensor.mac_address;
    const params = new URLSearchParams({ sensor_id: sensorId });

    if (start) params.set('start', start);

    if (bucket === 'raw') {
        params.set('limit', '5000');
        const resp = await fetch(`${window.__READINGS_ENDPOINT}?${params}`);
        if (!resp.ok) return null;
        const readings = await resp.json();
        readings.reverse();
        return readings.map(r => ({
            temperature_c: r.temperature_c,
            humidity_pct: r.humidity_pct,
            timestamp: r.timestamp,
        }));
    } else {
        params.set('bucket', bucket);
        const resp = await fetch(`${window.__AGGREGATE_ENDPOINT}?${params}`);
        if (!resp.ok) return null;
        const buckets = await resp.json();
        return buckets.map(b => ({
            temperature_c: b.avg_temp,
            humidity_pct: b.avg_humidity,
            min_temp: b.min_temp,
            max_temp: b.max_temp,
            timestamp: b.bucket,
        }));
    }
}

async function updateCharts() {
    const start = getTimeRangeStart(activeRange);
    const rangeConfig = TIME_RANGES[activeRange];

    const fetchPromises = [];
    const sensorIndices = [];

    sensors.forEach((sensor, index) => {
        const sensorId = sensor.id || sensor.mac_address;
        if (enabledSensors.has(sensorId)) {
            fetchPromises.push(fetchSensorData(sensor, start, rangeConfig.bucket));
            sensorIndices.push(index);
        }
    });

    const results = await Promise.all(fetchPromises);

    const tempDatasets = [];
    const humidityDatasets = [];

    results.forEach((data, i) => {
        if (!data || data.length === 0) return;

        const sensorIndex = sensorIndices[i];
        const sensor = sensors[sensorIndex];
        const color = getColor(sensorIndex);
        const label = sensor.name || sensor.mac_address || `Sensor ${sensorIndex + 1}`;

        const tempData = data.map(d => ({
            x: new Date(d.timestamp).getTime(),
            y: parseFloat(d.temperature_c.toFixed(1)),
        })).filter(d => d.y !== null && !isNaN(d.y));

        const humidityData = data.map(d => ({
            x: new Date(d.timestamp).getTime(),
            y: d.humidity_pct !== null && d.humidity_pct !== undefined ? parseFloat(d.humidity_pct.toFixed(1)) : null,
        })).filter(d => d.y !== null && !isNaN(d.y));

        const isRaw = rangeConfig.bucket === 'raw';
        const segConfig = isRaw ? { segment: segmentConfig(activeRange) } : {};

        if (tempData.length > 0) {
            tempDatasets.push({
                label,
                data: tempData,
                borderColor: color,
                backgroundColor: color + '1A',
                fill: false,
                tension: 0.3,
                pointRadius: 2,
                pointHoverRadius: 5,
                borderWidth: 2,
                ...segConfig,
            });
        }

        if (humidityData.length > 0) {
            humidityDatasets.push({
                label,
                data: humidityData,
                borderColor: color,
                backgroundColor: color + '33',
                fill: false,
                tension: 0.3,
                pointRadius: 2,
                pointHoverRadius: 5,
                borderWidth: 2,
                ...segConfig,
            });
        }
    });

    updateChart('temp-chart', tempDatasets, 'Temperature (°C)', {
        suggestedMin: null,
        suggestedMax: null,
    });
    updateChart('humidity-chart', humidityDatasets, 'Humidity (%)', {
        suggestedMin: 0,
        suggestedMax: 100,
    });
}

function getChartDefaults() {
    const isDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    return {
        gridColor: isDark ? '#374151' : '#E5E7EB',
        textColor: isDark ? '#D1D5DB' : '#6B7280',
    };
}

function createOrUpdateChart(canvasId, datasets, yLabel, yScaleOptions) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return null;

    const ctx = canvas.getContext('2d');
    const defaults = getChartDefaults();

    const config = {
        type: 'line',
        data: { datasets },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: {
                intersect: false,
                mode: 'index',
            },
            plugins: {
                legend: {
                    display: datasets.length > 1,
                    position: 'bottom',
                    labels: {
                        padding: 16,
                        usePointStyle: true,
                        pointStyle: 'circle',
                        color: defaults.textColor,
                        font: { size: 12 },
                    },
                },
                tooltip: {
                    backgroundColor: 'rgba(17, 24, 39, 0.9)',
                    titleColor: '#F9FAFB',
                    bodyColor: '#D1D5DB',
                    padding: 10,
                    cornerRadius: 8,
                    displayColors: true,
                    boxPadding: 4,
                    callbacks: {
                        title(items) {
                            if (!items.length) return '';
                            const d = new Date(items[0].parsed.x);
                            return d.toLocaleString();
                        },
                    },
                },
            },
            scales: {
                x: {
                    type: 'time',
                    time: {
                        tooltipFormat: 'MMM d, HH:mm',
                        displayFormats: {
                            minute: 'HH:mm',
                            hour: 'MMM d HH:mm',
                            day: 'MMM d',
                        },
                    },
                    grid: {
                        color: defaults.gridColor,
                        drawBorder: false,
                    },
                    ticks: {
                        color: defaults.textColor,
                        maxTicksLimit: 12,
                        font: { size: 11 },
                    },
                },
                y: {
                    beginAtZero: false,
                    grid: {
                        color: defaults.gridColor,
                        drawBorder: false,
                    },
                    ticks: {
                        color: defaults.textColor,
                        font: { size: 11 },
                        callback(value) {
                            return yLabel === 'Temperature (°C)' ? value + '°C' : value + '%';
                        },
                    },
                    title: {
                        display: true,
                        text: yLabel,
                        color: defaults.textColor,
                        font: { size: 12, weight: '600' },
                    },
                    ...yScaleOptions,
                },
            },
        },
    };

    if (Chart.getChart(canvasId)) {
        const existing = Chart.getChart(canvasId);
        existing.data.datasets = datasets;
        existing.update('none');
        return existing;
    }

    return new Chart(ctx, config);
}

function updateChart(canvasId, datasets, yLabel, yScaleOptions) {
    const chart = createOrUpdateChart(canvasId, datasets, yLabel, yScaleOptions);
    if (canvasId === 'temp-chart') tempChart = chart;
    if (canvasId === 'humidity-chart') humidityChart = chart;
}

document.addEventListener('DOMContentLoaded', () => {
    buildSensorCheckboxes();
    setupTimeRangeButtons();
    updateCharts();
});
