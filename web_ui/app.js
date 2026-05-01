'use strict';

/* ---- State --------------------------------------------------------------- */

let autoControlEnabled   = true;
let shutterLocked        = {};      // slot -> { expectedStatus, timer }
let scanning             = false;
let cameraStatusLoaded   = false;
let pollTimer            = null;    // 1s BLE scan poll
let countdownTimer       = null;    // 1s scan countdown display
let scanSecondsLeft      = 0;
let modalPairedRefreshTimer = null;

/* ---- Helpers ------------------------------------------------------------- */

function setStatus(msg) {
    document.getElementById('status').textContent = msg;
}

async function apiFetch(method, path, body) {
    const opts = { method, headers: {} };
    if (body !== undefined) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
    }
    const r = await fetch(path, opts);
    if (!r.ok) throw new Error(`${method} ${path} → ${r.status}`);
    const text = await r.text();
    return text ? JSON.parse(text) : {};
}

/* ---- Timezone dropdown --------------------------------------------------- */

function buildTimezoneDropdown() {
    const sel = document.getElementById('tz-select');
    for (let h = -12; h <= 14; h++) {
        const opt = document.createElement('option');
        opt.value = h;
        opt.textContent = h === 0 ? 'UTC' : (h > 0 ? `UTC+${h}` : `UTC${h}`);
        sel.appendChild(opt);
    }
    sel.addEventListener('change', () => {
        apiFetch('POST', '/api/settings/timezone', { tz_offset_hours: parseInt(sel.value) })
            .catch(() => {});
    });
}

/* ---- Auto-control toggle ------------------------------------------------- */

function applyAutoControl(enabled) {
    autoControlEnabled = enabled;

    const track  = document.getElementById('toggle-track');
    const state  = document.getElementById('toggle-state');
    const bar    = document.getElementById('control-bar');

    if (enabled) {
        track.classList.add('on');
        state.textContent  = 'On';
        state.style.color  = 'var(--green)';
        bar.style.display  = 'none';
    } else {
        track.classList.remove('on');
        state.textContent  = 'Off';
        state.style.color  = 'var(--gray-light)';
        bar.style.display  = 'grid';
    }

    // Re-render camera cards so per-camera shutter buttons appear/disappear
    renderCameraCards(lastCameraList);
}

function setAutoControl(enabled) {
    apiFetch('POST', '/api/auto-control', { enabled })
        .then(d => applyAutoControl(d.enabled))
        .catch(() => {});
}

document.getElementById('toggle-wrap').addEventListener('click', () => {
    setAutoControl(!autoControlEnabled);
});

/* ---- Camera status ------------------------------------------------------- */

let lastCameraList = [];

const STATUS_LABEL = {
    disconnected:  'Not Connected',
    connected:     'Connected',
    not_recording: 'Not Recording',
    recording:     'Recording',
};

function statusBadgeClass(status) {
    return status === 'not_recording' ? 'not-recording' : status;
}

function makeBadge(status) {
    const cls = statusBadgeClass(status);
    return `<div class="status-badge ${cls}">
        <span class="status-dot"></span>
        <span>${STATUS_LABEL[status] || status}</span>
    </div>`;
}

function makeShutterBtn(cam) {
    if (autoControlEnabled) return '';
    if (cam.status !== 'not_recording' && cam.status !== 'recording') return '';
    const isRec = cam.status === 'recording';
    const lock  = shutterLocked[cam.slot];
    const dis   = lock ? 'disabled' : '';
    const cls   = isRec ? 'cam-shutter-stop' : 'cam-shutter-start';
    const label = isRec ? 'Stop' : 'Record';
    return `<button class="cam-shutter-btn ${cls}" ${dis}
        data-slot="${cam.slot}" data-on="${isRec ? 'false' : 'true'}">${label}</button>`;
}

function renderCameraCards(cameras) {
    lastCameraList = cameras;

    const list    = document.getElementById('cam-status-list');
    const loading = document.getElementById('cam-status-loading');
    const empty   = document.getElementById('cam-status-empty');

    loading.style.display = 'none';

    if (!cameras.length) {
        empty.style.display = 'block';
        // Remove existing cards
        list.querySelectorAll('.camera-card').forEach(el => el.remove());
        return;
    }

    empty.style.display = 'none';

    cameras.forEach(cam => {
        const id   = `cam-card-${cam.slot}`;
        let   card = document.getElementById(id);

        const typeBadge = cam.type === 'rc_emulation'
            ? '<span class="cam-type-badge">WiFi RC</span>' : '';

        const modelLine = (cam.model_name && cam.model_name !== cam.name)
            ? `<div class="cam-model-name">${cam.model_name}</div>` : '';

        const inner = `
            <div class="cam-meta">
                <span class="cam-number">Cam ${cam.index}</span>
                <span class="cam-display-name">${cam.name}</span>
                ${typeBadge}
            </div>
            ${modelLine}
            <div class="cam-footer">
                ${makeBadge(cam.status)}
                ${makeShutterBtn(cam)}
            </div>`;

        if (!card) {
            card = document.createElement('div');
            card.className = 'camera-card';
            card.id = id;
            list.appendChild(card);
        }
        card.innerHTML = inner;
    });

    // Remove cards for slots no longer present
    list.querySelectorAll('.camera-card').forEach(el => {
        const slot = parseInt(el.id.replace('cam-card-', ''));
        if (!cameras.find(c => c.slot === slot)) el.remove();
    });
}

function refreshCameraStatus() {
    apiFetch('GET', '/api/paired-cameras')
        .then(cameras => {
            cameraStatusLoaded = true;

            // Resolve shutter locks early if camera reached expected state
            cameras.forEach(cam => {
                const lock = shutterLocked[cam.slot];
                if (lock && cam.status === lock.expectedStatus) {
                    clearTimeout(lock.timer);
                    delete shutterLocked[cam.slot];
                }
            });

            renderCameraCards(cameras);
        })
        .catch(() => {});
}

// Shutter button delegation
document.getElementById('cam-status-list').addEventListener('click', e => {
    const btn = e.target.closest('.cam-shutter-btn');
    if (!btn || btn.disabled) return;
    const slot = parseInt(btn.dataset.slot);
    const on   = btn.dataset.on === 'true';
    const expectedStatus = on ? 'recording' : 'not_recording';

    btn.disabled = true;
    shutterLocked[slot] = {
        expectedStatus,
        timer: setTimeout(() => { delete shutterLocked[slot]; renderCameraCards(lastCameraList); }, 5000),
    };

    apiFetch('POST', '/api/shutter', { slot, on })
        .then(d => setStatus(`Shutter command sent (${d.dispatched} camera${d.dispatched !== 1 ? 's' : ''}).`))
        .catch(() => setStatus('Shutter command failed.'));
});

// Record All / Stop All
document.getElementById('btn-record-all').addEventListener('click', function () {
    this.disabled = true;
    document.getElementById('btn-stop-all').disabled = true;
    apiFetch('POST', '/api/shutter', { on: true })
        .then(d => setStatus(`Record All sent (${d.dispatched} camera${d.dispatched !== 1 ? 's' : ''}).`))
        .catch(() => setStatus('Record All failed.'))
        .finally(() => {
            document.getElementById('btn-record-all').disabled = false;
            document.getElementById('btn-stop-all').disabled   = false;
        });
});

document.getElementById('btn-stop-all').addEventListener('click', function () {
    this.disabled = true;
    document.getElementById('btn-record-all').disabled = true;
    apiFetch('POST', '/api/shutter', { on: false })
        .then(d => setStatus(`Stop All sent (${d.dispatched} camera${d.dispatched !== 1 ? 's' : ''}).`))
        .catch(() => setStatus('Stop All failed.'))
        .finally(() => {
            document.getElementById('btn-record-all').disabled = false;
            document.getElementById('btn-stop-all').disabled   = false;
        });
});

/* ---- RC status + UTC + auto-control poll --------------------------------- */

function refreshTopSection() {
    apiFetch('GET', '/api/logging-state').then(d => {
        const pill = document.getElementById('rc-logging-pill');
        const cls  = 'rc-' + d.state.replace('_', '-');
        const labels = { logging: 'Logging', not_logging: 'Not Logging', unknown: 'Unknown' };
        pill.className = 'rc-value ' + cls;
        pill.textContent = labels[d.state] || d.state;
    }).catch(() => {});

    apiFetch('GET', '/api/utc').then(d => {
        const dateLine = document.getElementById('utc-date-line');
        const timeLine = document.getElementById('utc-time-line');
        if (!d.valid) {
            dateLine.textContent = 'No GPS';
            timeLine.textContent = '';
        } else {
            const dt = new Date(d.epoch_ms);
            dateLine.textContent = `${dt.getUTCFullYear()}-${String(dt.getUTCMonth()+1).padStart(2,'0')}-${String(dt.getUTCDate()).padStart(2,'0')}`;
            timeLine.textContent = `${String(dt.getUTCHours()).padStart(2,'0')}:${String(dt.getUTCMinutes()).padStart(2,'0')}:${String(dt.getUTCSeconds()).padStart(2,'0')}`;
        }
    }).catch(() => {});

    apiFetch('GET', '/api/auto-control').then(d => {
        applyAutoControl(d.enabled);
    }).catch(() => {});
}

/* ---- Settings modal ------------------------------------------------------ */

const settingsOverlay = document.getElementById('settings-overlay');

document.getElementById('settings-btn').addEventListener('click', openSettings);
document.getElementById('settings-done').addEventListener('click', closeSettings);
settingsOverlay.addEventListener('click', e => { if (e.target === settingsOverlay) closeSettings(); });

function openSettings() {
    settingsOverlay.classList.add('open');

    // Load current timezone
    apiFetch('GET', '/api/settings/timezone').then(d => {
        document.getElementById('tz-select').value = d.tz_offset_hours;
    }).catch(() => {});

    // Show Set Date & Time row only when GPS is not valid
    apiFetch('GET', '/api/utc').then(d => {
        document.getElementById('datetime-row').style.display = d.valid ? 'none' : 'flex';
    }).catch(() => {});
}

function closeSettings() {
    settingsOverlay.classList.remove('open');
}

// Set Date & Time from browser — use event delegation so the button survives innerHTML replacement
document.getElementById('settings-overlay').addEventListener('click', e => {
    if (e.target.id !== 'datetime-btn') return;
    const actionDiv = document.getElementById('datetime-action');
    document.getElementById('datetime-btn').disabled = true;
    apiFetch('POST', '/api/settings/datetime', { epoch_ms: Date.now() })
        .then(() => {
            actionDiv.innerHTML = '<span class="settings-inline-msg" style="color:var(--green)">Time set ✓</span>';
            setTimeout(() => {
                actionDiv.innerHTML = '<button class="settings-action-btn" id="datetime-btn">Set from Device</button>';
            }, 2000);
        })
        .catch(() => {
            actionDiv.innerHTML = '<span class="settings-inline-msg" style="color:var(--red)">Failed — try again</span>';
            setTimeout(() => {
                actionDiv.innerHTML = '<button class="settings-action-btn" id="datetime-btn">Set from Device</button>';
            }, 2000);
        });
});

// Reboot
document.getElementById('reboot-btn').addEventListener('click', function () {
    if (!confirm('Reboot Controller?\n\nThe device will restart. Paired cameras and settings will be preserved.')) return;
    this.disabled = true;
    this.textContent = 'Rebooting…';
    apiFetch('POST', '/api/reboot').catch(() => {});
    setTimeout(() => location.reload(), 5000);
});

// Factory reset
document.getElementById('reset-btn').addEventListener('click', function () {
    if (!confirm('Restore Defaults?\n\nThis will erase all paired cameras and settings, then restart the controller. This cannot be undone.')) return;
    this.disabled = true;
    this.textContent = 'Resetting…';
    apiFetch('POST', '/api/factory-reset').catch(() => {});
    setTimeout(() => location.reload(), 5000);
});

/* ---- Manage cameras modal ------------------------------------------------ */

const modalOverlay = document.getElementById('modal-overlay');

document.getElementById('manage-btn').addEventListener('click', openModal);
document.getElementById('modal-done').addEventListener('click', closeModal);
modalOverlay.addEventListener('click', e => { if (e.target === modalOverlay) closeModal(); });

function openModal() {
    modalOverlay.classList.add('open');
    refreshModalPairedCameras();
    refreshRcDiscovered();
    modalPairedRefreshTimer = setInterval(() => {
        refreshModalPairedCameras();
        refreshRcDiscovered();
    }, 3000);
}

function closeModal() {
    if (scanning) cancelScan();
    clearInterval(modalPairedRefreshTimer);
    modalPairedRefreshTimer = null;
    document.getElementById('modal-status').textContent = '';
    document.getElementById('results').innerHTML = '';
    modalOverlay.classList.remove('open');
}

function setModalStatus(msg) {
    document.getElementById('modal-status').textContent = msg;
}

/* ---- BLE Scan ------------------------------------------------------------ */

document.getElementById('scan-btn').addEventListener('click', () => {
    if (scanning) {
        cancelScan();
    } else {
        startScan();
    }
});

function startScan() {
    scanning = true;
    scanSecondsLeft = 120;
    const btn = document.getElementById('scan-btn');
    btn.textContent = 'Cancel Scan';
    btn.classList.add('scanning');
    setModalStatus(`Scanning… ${scanSecondsLeft}s`);

    apiFetch('POST', '/api/scan').catch(() => {});

    pollTimer = setInterval(pollScanResults, 1000);
    countdownTimer = setInterval(() => {
        scanSecondsLeft--;
        if (scanSecondsLeft > 0) {
            setModalStatus(`Scanning… ${scanSecondsLeft}s`);
        } else {
            stopScan(false);
        }
    }, 1000);
}

function cancelScan() {
    apiFetch('POST', '/api/scan-cancel').catch(() => {});
    stopScan(true);
}

function stopScan(cancelled) {
    scanning = false;
    clearInterval(pollTimer);
    clearInterval(countdownTimer);
    pollTimer = null;
    countdownTimer = null;

    const btn = document.getElementById('scan-btn');
    btn.textContent = 'Scan for Cameras';
    btn.classList.remove('scanning');

    // Final poll
    pollScanResults().then(() => {
        setModalStatus(cancelled ? 'Scan cancelled.' : 'Scan complete.');
    });
}

let lastPairedAddrs = new Set();

function pollScanResults() {
    return Promise.all([
        apiFetch('GET', '/api/cameras'),
        apiFetch('GET', '/api/paired-cameras'),
    ]).then(([found, paired]) => {
        lastPairedAddrs = new Set(paired.map(c => c.addr));
        const unpaired = found.filter(c => !lastPairedAddrs.has(c.addr));
        renderFoundCameras(unpaired);
    }).catch(() => {});
}

function renderFoundCameras(cameras) {
    const results = document.getElementById('results');
    results.innerHTML = '';
    cameras.forEach(cam => {
        const row = document.createElement('div');
        row.className = 'found-camera-row';
        row.innerHTML = `
            <div class="found-cam-info">
                <div class="found-cam-name">${cam.name}</div>
                <div class="found-cam-meta">${cam.addr} &nbsp;·&nbsp; RSSI ${cam.rssi}</div>
            </div>
            <button class="pair-this-btn" data-addr="${cam.addr}" data-addr-type="${cam.addr_type}">Pair</button>`;
        results.appendChild(row);
    });
}

document.getElementById('results').addEventListener('click', e => {
    const btn = e.target.closest('.pair-this-btn');
    if (!btn) return;
    const addr      = btn.dataset.addr;
    const addr_type = btn.dataset.addrType;

    if (scanning) cancelScan();
    document.getElementById('results').innerHTML = '';
    setModalStatus('Pairing initiated — camera should appear in the list shortly.');

    apiFetch('POST', '/api/pair', { addr, addr_type }).catch(() => {});
});

/* ---- RC Emulation discovered --------------------------------------------- */

document.getElementById('rc-add-btn').addEventListener('click', refreshRcDiscovered);

function refreshRcDiscovered() {
    apiFetch('GET', '/api/rc/discovered')
        .then(renderRcDiscovered)
        .catch(() => {});
}

function renderRcDiscovered(devices) {
    const container = document.getElementById('rc-results');
    container.innerHTML = '';

    if (!devices.length) {
        const p = document.createElement('p');
        p.className = 'modal-empty';
        p.textContent = 'No unidentified devices connected.';
        container.appendChild(p);
        return;
    }

    const msg = document.createElement('p');
    msg.className = 'modal-empty';
    msg.textContent = `${devices.length} device${devices.length !== 1 ? 's' : ''} connected — click Add to probe:`;
    container.appendChild(msg);

    devices.forEach(dev => {
        const row = document.createElement('div');
        row.className = 'found-camera-row';
        const ip = dev.ip || null;
        row.innerHTML = `
            <div class="found-cam-info">
                <div class="found-cam-name">Unknown Device</div>
                <div class="found-cam-meta">${dev.addr} &nbsp;·&nbsp; ${ip || 'IP pending'}</div>
            </div>
            <button class="pair-this-btn" data-addr="${dev.addr}" data-ip="${ip || ''}">Add</button>`;
        container.appendChild(row);
    });
}

document.getElementById('rc-results').addEventListener('click', e => {
    const btn = e.target.closest('.pair-this-btn');
    if (!btn) return;
    const addr = btn.dataset.addr;
    const ip   = btn.dataset.ip;

    if (!ip) {
        setModalStatus('Cannot add — IP address not yet assigned. Wait a moment and click Refresh List.');
        return;
    }

    setModalStatus(`Probing device ${addr}… (up to 15 s)`);
    btn.disabled = true;
    document.getElementById('rc-add-btn').disabled = true;

    apiFetch('POST', '/api/rc/add', { addr, ip })
        .then(() => {
            setTimeout(() => {
                apiFetch('GET', '/api/rc/discovered').then(devices => {
                    const stillThere = devices.find(d => d.addr === addr);
                    if (stillThere) {
                        setModalStatus(`⚠️ Could not identify ${addr} as a GoPro camera.`);
                    } else {
                        setModalStatus('✅ Camera added — it will appear in the camera list shortly.');
                    }
                    renderRcDiscovered(devices);
                    document.getElementById('rc-add-btn').disabled = false;
                    refreshModalPairedCameras();
                }).catch(() => {
                    document.getElementById('rc-add-btn').disabled = false;
                });
            }, 15000);
        })
        .catch(() => {
            setModalStatus('Add request failed.');
            document.getElementById('rc-add-btn').disabled = false;
        });
});

/* ---- Paired cameras list in modal ---------------------------------------- */

function refreshModalPairedCameras() {
    apiFetch('GET', '/api/paired-cameras')
        .then(renderModalPairedCameras)
        .catch(() => {});
}

function renderModalPairedCameras(cameras) {
    const list  = document.getElementById('paired-list');
    const badge = document.getElementById('paired-count');
    list.innerHTML = '';

    if (!cameras.length) {
        badge.classList.remove('visible');
        const p = document.createElement('p');
        p.className = 'modal-empty';
        p.textContent = 'No cameras paired.';
        list.appendChild(p);
        return;
    }

    badge.textContent = cameras.length;
    badge.classList.add('visible');

    cameras.forEach(cam => {
        const isRc     = cam.type === 'rc_emulation';
        const typeBadge = isRc ? '<span class="cam-type-badge">WiFi RC</span>' : '';
        const removeLabel = isRc ? 'Remove' : 'Forget';
        const metaParts = [cam.model_name, `Cam ${cam.index}`];
        if (isRc && cam.addr) metaParts.push(cam.addr);

        const row = document.createElement('div');
        row.className = 'modal-paired-row';
        row.innerHTML = `
            <div class="modal-paired-info">
                <div class="modal-paired-name">${cam.name} ${typeBadge}</div>
                <div class="modal-paired-meta">${metaParts.filter(Boolean).join(' · ')}</div>
            </div>
            <button class="remove-btn" data-slot="${cam.slot}" data-rc="${isRc}">${removeLabel}</button>`;
        list.appendChild(row);
    });
}

document.getElementById('paired-list').addEventListener('click', e => {
    const btn = e.target.closest('.remove-btn');
    if (!btn) return;
    const slot = parseInt(btn.dataset.slot);
    const isRc = btn.dataset.rc === 'true';
    const verb = isRc ? 'Remove' : 'Forget';

    if (!confirm(`${verb} this camera?`)) return;

    apiFetch('POST', '/api/remove-camera', { slot })
        .then(() => {
            refreshModalPairedCameras();
            if (isRc) {
                setTimeout(refreshRcDiscovered, 1500);
            }
        })
        .catch(() => {});
});

/* ---- Startup ------------------------------------------------------------- */

buildTimezoneDropdown();
refreshTopSection();
refreshCameraStatus();

setInterval(refreshCameraStatus, 3000);
setInterval(refreshTopSection, 2000);
