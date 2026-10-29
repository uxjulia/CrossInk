let allSettings = [];
  let originalValues = {};
  let preserveQuickResumeTimeoutOn = false;
  let quickResumeTimeoutAutoEnabled = false;
  const SLEEP_SCREEN_MODE = {
    QUICK_RESUME: 6
  };

  function escapeHtml(unsafe) {
    return unsafe
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&#039;");
  }

  function showMessage(text, isError) {
    const msg = document.getElementById('message');
    clearTimeout(msg.hideTimer);
    msg.textContent = text;
    msg.className = 'message ' + (isError ? 'error' : 'success');
    msg.style.display = 'block';
    msg.hideTimer = setTimeout(function() { msg.style.display = 'none'; }, 4000);
  }

  function renderControl(setting) {
    const id = 'setting-' + setting.key;

    if (setting.type === 'toggle') {
      const checked = setting.value ? 'checked' : '';
      return '<label class="toggle-switch">' +
        '<input type="checkbox" id="' + id + '" ' + checked + ' onchange="handleSettingChanged(\'' + setting.key + '\')">' +
        '<span class="toggle-slider"></span></label>';
    }

    if (setting.type === 'enum') {
      let html = '<select id="' + id + '" onchange="handleSettingChanged(\'' + setting.key + '\')">';
      setting.options.forEach(function(opt, idx) {
        const selected = idx === setting.value ? ' selected' : '';
        html += '<option value="' + idx + '"' + selected + '>' + escapeHtml(opt) + '</option>';
      });
      html += '</select>';
      return html;
    }

    if (setting.type === 'value') {
      const wordSpacing = setting.key === 'wordSpacing';
      const value = wordSpacing && setting.value > 4 ? 4 - setting.value : setting.value;
      const minimum = wordSpacing ? -4 : setting.min;
      const maximum = wordSpacing ? 4 : setting.max;
      return '<input type="number" id="' + id + '" value="' + value + '"' +
        ' min="' + minimum + '" max="' + maximum + '" step="' + setting.step + '"' +
        ' onchange="handleSettingChanged(\'' + setting.key + '\')">';
    }

    if (setting.type === 'string') {
      const inputType = setting.name.toLowerCase().includes('password') ? 'password' : 'text';
      const val = setting.value || '';
      return '<input type="' + inputType + '" id="' + id + '" value="' + escapeHtml(val) + '"' +
        ' oninput="handleSettingChanged(\'' + setting.key + '\')">';
    }

    return '';
  }

  function getValue(setting) {
    const el = document.getElementById('setting-' + setting.key);
    if (!el) return undefined;

    if (setting.type === 'toggle') {
      return el.checked ? 1 : 0;
    }
    if (setting.type === 'enum') {
      return parseInt(el.value, 10);
    }
    if (setting.type === 'value') {
      const value = parseInt(el.value, 10);
      if (setting.key === 'wordSpacing' && Number.isFinite(value)) {
        const level = Math.max(-4, Math.min(4, value));
        el.value = String(level);
        return level < 0 ? 4 - level : level;
      }
      return value;
    }
    if (setting.type === 'string') {
      return el.value;
    }
    return undefined;
  }

  function markChanged() {
    document.getElementById('saveBtn').disabled = false;
  }

  function findSetting(key) {
    return allSettings.find(function(s) { return s.key === key; });
  }

  function getControl(key) {
    return document.getElementById('setting-' + key);
  }

  function getControlValue(key) {
    const setting = findSetting(key);
    const el = getControl(key);
    if (!setting || !el) return undefined;

    if (setting.type === 'toggle') {
      return el.checked ? 1 : 0;
    }
    if (setting.type === 'enum' || setting.type === 'value') {
      return parseInt(el.value, 10);
    }
    return el.value;
  }

  function setControlValue(key, value) {
    const setting = findSetting(key);
    const el = getControl(key);
    if (!setting || !el) return false;

    if (setting.type === 'toggle') {
      const checked = !!value;
      if (el.checked === checked) return false;
      el.checked = checked;
      return true;
    }

    const nextValue = String(value);
    if (el.value === nextValue) return false;
    el.value = nextValue;
    return true;
  }

  function isQuickResumeSleepScreenSelected() {
    const sleepScreen = findSetting('sleepScreen');
    const sleepScreenValue = getControlValue('sleepScreen');
    if (!sleepScreen || sleepScreenValue === undefined || !Number.isFinite(sleepScreenValue)) return false;

    let selectedValue = sleepScreenValue;
    if (Array.isArray(sleepScreen.values)) {
      selectedValue = Number(sleepScreen.values[sleepScreenValue]);
    } else if (Array.isArray(sleepScreen.options)) {
      const selectedOption = sleepScreen.options[sleepScreenValue];
      if (selectedOption && typeof selectedOption === 'object' && 'value' in selectedOption) {
        selectedValue = Number(selectedOption.value);
      }
    }

    return selectedValue === SLEEP_SCREEN_MODE.QUICK_RESUME;
  }

  function syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged) {
    const timeoutValue = getControlValue('quickResumeSleepScreen');
    if (timeoutValue === undefined) return false;

    let changed = false;
    if (quickResumeTimeoutChanged) {
      preserveQuickResumeTimeoutOn = timeoutValue === 1;
      quickResumeTimeoutAutoEnabled = false;
    }

    if (isQuickResumeSleepScreenSelected()) {
      if (timeoutValue !== 1) {
        changed = setControlValue('quickResumeSleepScreen', 1);
        quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
      } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
        quickResumeTimeoutAutoEnabled = true;
      }
      return changed;
    }

    if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
      changed = setControlValue('quickResumeSleepScreen', 0);
      quickResumeTimeoutAutoEnabled = false;
    }
    return changed;
  }

  function updateSettingsVisibility() {
    const pwrBtnFootnoteBackRow = document.getElementById('row-setting-pwrBtnFootnoteBack');
    if (pwrBtnFootnoteBackRow) {
      const footnoteActionSelected = ['shortPwrBtn', 'longPwrBtn', 'longPressMenuAction', 'longPressBackAction']
        .some(key => {
          const setting = allSettings.find(item => item.key === key);
          return setting && getControlValue(key) === setting.footnotesIndex;
        });
      if (footnoteActionSelected) {
        pwrBtnFootnoteBackRow.style.display = '';
      } else {
        pwrBtnFootnoteBackRow.style.display = 'none';
      }
    }
  }

  function handleSettingChanged(key) {
    syncQuickResumeTimeoutForSleepScreen(key === 'sleepScreen', key === 'quickResumeSleepScreen');
    if (key === 'shortPwrBtn' || key === 'longPwrBtn' || key === 'longPressMenuAction' ||
        key === 'longPressBackAction') {
      updateSettingsVisibility();
    }
    markChanged();
  }

  async function loadSettings() {
    try {
      const response = await fetch('/api/settings');
      if (!response.ok) {
        throw new Error('Failed to load settings: ' + response.status);
      }
      allSettings = await response.json();

      // Store original values
      originalValues = {};
      allSettings.forEach(function(s) {
        originalValues[s.key] = s.value;
      });

      // Group by category
      const groups = {};
      allSettings.forEach(function(s) {
        if (!groups[s.category]) groups[s.category] = [];
        groups[s.category].push(s);
      });

      const container = document.getElementById('settings-container');
      let html = '';

      for (const category in groups) {
        html += '<div class="card"><h2>' + escapeHtml(category) + '</h2>';
        groups[category].forEach(function(s) {
          if (s.key === 'opdsDownloadFolder') return;
          html += '<div class="setting-row" id="row-setting-' + s.key + '">' +
            '<span class="setting-name">' + escapeHtml(s.name) + '</span>' +
            '<span class="setting-control">' + renderControl(s) + '</span>' +
            '</div>';
        });
        html += '</div>';
      }

      container.innerHTML = html;
      updateSettingsVisibility();
      const folderControl = document.getElementById('setting-opdsDownloadFolder');
      if (folderControl) {
        folderControl.value = originalValues.opdsDownloadFolder || '';
        updateOpdsDestinations();
      }
      document.getElementById('save-container').style.display = '';
      document.getElementById('saveBtn').disabled = true;
      preserveQuickResumeTimeoutOn = getControlValue('quickResumeSleepScreen') === 1;
      quickResumeTimeoutAutoEnabled = false;
      if (syncQuickResumeTimeoutForSleepScreen(true, false)) {
        markChanged();
      }
    } catch (e) {
      console.error(e);
      document.getElementById('settings-container').innerHTML =
        '<div class="card"><p style="text-align:center;color:#e74c3c;">Failed to load settings</p></div>';
    }
  }

  async function saveSettings() {
    const btn = document.getElementById('saveBtn');
    btn.disabled = true;
    btn.textContent = 'Saving...';

    // Collect only changed values
    const changes = {};
    allSettings.forEach(function(s) {
      const current = getValue(s);
      if (current !== undefined && current !== originalValues[s.key]) {
        changes[s.key] = current;
      }
    });

    if (Object.prototype.hasOwnProperty.call(changes, 'koMatchMethod') &&
        !await showWebConfirm('Change Document Matching? Books get new ids, so the next sync checks every book again. ' +
                 'Progress already uploaded stays under the old ids until you merge books in the web app.')) {
      delete changes.koMatchMethod;
      setControlValue('koMatchMethod', originalValues.koMatchMethod);
    }

    if (Object.keys(changes).length === 0) {
      showMessage('No changes to save.', false);
      btn.textContent = 'Save Settings';
      return;
    }

    try {
      const response = await fetch('/api/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(changes)
      });

      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || 'Save failed');
      }

      // Update original values to new values
      for (const key in changes) {
        originalValues[key] = changes[key];
      }
      if (Object.prototype.hasOwnProperty.call(changes, 'trackReadingStats')) {
        await loadSettings();
      }
      updateOpdsDestinations();

      showMessage('Settings saved successfully!', false);
    } catch (e) {
      console.error(e);
      showMessage('Error: ' + e.message, true);
    }

    btn.textContent = 'Save Settings';
  }

  // Reader bars use one complete request so the two layouts change together.
  let statusBars = null;

  function statusBarSelect(id, choices, value, changed) {
    return '<select id="' + id + '" onchange="' + changed + '">' +
      choices.map(function(choice) {
        return '<option value="' + choice.value + '"' + (choice.value === value ? ' selected' : '') +
          (choice.disabled ? ' disabled' : '') + '>' + escapeHtml(String(choice.label)) + '</option>';
      }).join('') + '</select>';
  }

  function statusBarRow(label, control, rowId) {
    return '<div class="setting-row"' + (rowId ? ' id="' + rowId + '"' : '') + '><span class="setting-name">' + escapeHtml(label) +
      '</span><span class="setting-control">' + control + '</span></div>';
  }

  function renderStatusBar(position) {
    const bar = statusBars[position];
    const labels = statusBars.labels;
    const itemOptions = statusBars.options.map(function(option) {
      return { value: option.value, label: option.label,
        disabled: (option.value === 1 || option.value === 10) && !statusBars.clockAvailable };
    });
    const slotNames = [labels.left + ' 1', labels.left + ' 2', labels.left + ' 3', labels.center,
      labels.right + ' 1', labels.right + ' 2', labels.right + ' 3'];
    let html = '<div class="status-bar-editor"><h3>' + escapeHtml(labels[position]) + '</h3>';
    for (let i = 0; i < bar.slots.length; i++) {
      html += statusBarRow(slotNames[i], statusBarSelect('bar-' + position + '-slot-' + i,
        itemOptions, bar.slots[i], 'statusBarChanged()'));
    }
    for (const field of [
      ['battery', labels.battery, statusBars.batteryStyles],
      ['percentageFormat', labels.percentageFormat, statusBars.percentageFormats],
      ['progressBar', labels.progressBar, statusBars.progressModes],
      ['thickness', labels.thickness, statusBars.thicknesses],
      ['hidden', labels.hidden, statusBars.hideOptions]
    ]) {
      html += statusBarRow(field[1], statusBarSelect('bar-' + position + '-' + field[0],
        field[2].map(function(label, index) { return { value: index, label: label }; }),
        Number(bar[field[0]]), 'statusBarChanged()'));
    }
    html += statusBarRow(labels.centered, statusBarSelect('bar-' + position + '-centered',
      statusBars.hideOptions.map(function(label, index) { return { value: index, label: label }; }),
      bar.centered ? 1 : 0, 'statusBarChanged()'));
    html += statusBarRow(labels.separator, statusBarSelect('bar-' + position + '-separator',
      statusBars.separators.map(function(glyph, index) { return { value: index, label: glyph }; }),
      Number(bar.separator), 'statusBarChanged()'), 'bar-' + position + '-separator-row');
    html += '<div class="status-bar-preview" id="bar-' + position + '-preview"></div></div>';
    return html;
  }

  function renderDisplayStatusBar() {
    const choices = statusBars.options.filter(function(option) {
      return [0, 1, 2, 10].includes(option.value);
    }).map(function(option) {
      return { value: option.value, label: option.label,
        disabled: (option.value === 1 || option.value === 10) && !statusBars.clockAvailable };
    });
    return '<h3>' + escapeHtml(statusBars.labels.display) + '</h3>' +
      [statusBars.labels.left, statusBars.labels.center, statusBars.labels.right].map(function(label, index) {
        return statusBarRow(label, statusBarSelect('display-slot-' + index, choices,
          statusBars.display[index], 'statusBarChanged()'));
      }).join('') + statusBarRow(statusBars.labels.battery, statusBarSelect('display-battery',
        statusBars.batteryStyles.map(function(label, index) { return { value: index, label: label }; }),
        statusBars.displayBatteryStyle, 'statusBarChanged()')) +
      statusBarRow(statusBars.labels.textSize, statusBarSelect('display-text-size',
        statusBars.textSizes.map(function(label, index) { return { value: index, label: label }; }),
        statusBars.displayTextSize, 'statusBarChanged()'));
  }

  function readStatusBarForm(position) {
    const bar = statusBars[position];
    return {
      slots: bar.slots.map(function(_, index) {
        return Number(document.getElementById('bar-' + position + '-slot-' + index).value);
      }),
      battery: Number(document.getElementById('bar-' + position + '-battery').value),
      percentageFormat: Number(document.getElementById('bar-' + position + '-percentageFormat').value),
      progressBar: Number(document.getElementById('bar-' + position + '-progressBar').value),
      thickness: Number(document.getElementById('bar-' + position + '-thickness').value),
      hidden: document.getElementById('bar-' + position + '-hidden').value === '1',
      centered: document.getElementById('bar-' + position + '-centered').value === '1',
      separator: Number(document.getElementById('bar-' + position + '-separator').value)
    };
  }

  function updateStatusBarPreview(position) {
    const bar = readStatusBarForm(position);
    // The separator only applies to a centered bar.
    document.getElementById('bar-' + position + '-separator-row').style.display = bar.centered ? '' : 'none';
    if (bar.hidden) {
      document.getElementById('bar-' + position + '-preview').innerHTML = '';
      return;
    }
    const battery = ['\u25AD 85%', '\u25AD', '85%'][bar.battery] || '85%';
    const examples = ['', '10:30', battery, '2h 15m', '12m', '4/12', '27',
      (64.12).toFixed(bar.percentageFormat) + '%', 'Book title', 'Chapter title', statusBars.datePreview,
      (35.25).toFixed(bar.percentageFormat) + '%'];
    const slot = function(index) { return escapeHtml(examples[bar.slots[index]] || ''); };
    const progressValue = bar.progressBar === 0 ? 64 : 35;
    const progress = bar.progressBar === 2 ? '' : '<div class="status-bar-preview-progress" style="height:' +
      ((bar.thickness + 1) * 2) + 'px"><span style="width:' + progressValue + '%"></span></div>';
    if (bar.centered) {
      const joined = [0, 1, 2, 3, 4, 5, 6].map(slot).filter(Boolean)
        .join(' ' + escapeHtml(statusBars.separators[bar.separator] || '') + ' ');
      document.getElementById('bar-' + position + '-preview').innerHTML =
        (position === 'top' ? progress : '') + '<div class="status-bar-preview-items status-bar-preview-centered">' +
        '<span>' + joined + '</span></div>' + (position === 'bottom' ? progress : '');
      return;
    }
    const right = [4, 5, 6].map(slot).filter(Boolean).join(' &nbsp; ');
    document.getElementById('bar-' + position + '-preview').innerHTML =
      (position === 'top' ? progress : '') + '<div class="status-bar-preview-items"><span>' +
      [slot(0), slot(1), slot(2)].filter(Boolean).join(' &nbsp; ') + '</span><span>' + slot(3) +
      '</span><span>' + right + '</span></div>' + (position === 'bottom' ? progress : '');
  }

  function statusBarChanged() {
    document.getElementById('statusBarsSaveBtn').disabled = false;
    updateStatusBarPreview('top');
    updateStatusBarPreview('bottom');
  }

  async function loadStatusBars() {
    const container = document.getElementById('status-bars-container');
    try {
      const response = await fetch('/api/status-bars');
      if (!response.ok) throw new Error('Failed to load status bars');
      statusBars = await response.json();
      container.innerHTML = '<div class="card"><h2>' + escapeHtml(statusBars.labels.top) + ' / ' +
        escapeHtml(statusBars.labels.bottom) + '</h2>' + renderStatusBar('top') +
        renderStatusBar('bottom') + renderDisplayStatusBar() +
        statusBarRow(statusBars.labels.xtcMode, statusBarSelect('bar-xtc-mode',
          statusBars.xtcModes.map(function(label, index) { return { value: index, label: label }; }),
          statusBars.xtcMode, 'statusBarChanged()')) +
        '<div class="save-container"><button class="save-btn" id="statusBarsSaveBtn" ' +
        'onclick="saveStatusBars()" disabled>Save Status Bars</button></div></div>';
      updateStatusBarPreview('top');
      updateStatusBarPreview('bottom');
    } catch (error) {
      console.error(error);
      container.innerHTML = '<div class="card"><p>Failed to load status bars</p></div>';
    }
  }

  async function saveStatusBars() {
    const btn = document.getElementById('statusBarsSaveBtn');
    btn.disabled = true;
    try {
      const response = await fetch('/api/status-bars', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ top: readStatusBarForm('top'), bottom: readStatusBarForm('bottom'),
          display: statusBars.display.map(function(_, index) {
            return Number(document.getElementById('display-slot-' + index).value);
          }),
          displayBatteryStyle: Number(document.getElementById('display-battery').value),
          displayTextSize: Number(document.getElementById('display-text-size').value),
          xtcMode: Number(document.getElementById('bar-xtc-mode').value) })
      });
      if (!response.ok) throw new Error(await response.text());
      showMessage('Status bars saved.', false);
      await loadStatusBars();
    } catch (error) {
      btn.disabled = false;
      showMessage('Error: ' + error.message, true);
    }
  }

  // --- Wi-Fi Network Management ---
  // Renders an editable list of saved Wi-Fi networks using /api/wifi endpoints.
  // Password fields are never pre-filled; when left blank during edit, existing
  // passwords remain unchanged server-side.
  let wifiNetworks = [];

  function renderWifiNetwork(net, idx) {
    const isNew = idx === -1;
    const id = isNew ? 'new' : idx;
    const lastConnected = net.isLastConnected
      ? '<div style="margin-top:8px;color:var(--label-color);font-size:0.9em;">Last connected network</div>'
      : '';

    return '<div class="opds-server" id="wifi-' + id + '">' +
      '<div class="setting-row">' +
        '<span class="setting-name">SSID</span>' +
        '<span class="setting-control"><input type="text" id="wifi-ssid-' + id + '" value="' + escapeHtml(net.ssid || '') + '"></span>' +
      '</div>' +
      '<div class="setting-row">' +
        '<span class="setting-name">Password</span>' +
        '<span class="setting-control"><input type="password" id="wifi-pass-' + id + '" placeholder="' + (net.hasPassword ? '(unchanged)' : '') + '"></span>' +
      '</div>' +
      lastConnected +
      '<div class="opds-actions">' +
        '<button class="btn-small btn-save-server" onclick="saveWifiNetwork(' + idx + ')">Save</button>' +
        (isNew ? '' : '<button class="btn-small btn-delete" onclick="deleteWifiNetwork(' + idx + ')">Delete</button>') +
      '</div>' +
    '</div>';
  }

  function renderWifiSection() {
    const container = document.getElementById('wifi-container');
    let html = '<div class="card"><h2>Wi-Fi Networks</h2>';

    if (wifiNetworks.length === 0) {
      html += '<p style="color:var(--label-color);text-align:center;">No Wi-Fi networks saved</p>';
    } else {
      wifiNetworks.forEach(function(net, idx) {
        html += renderWifiNetwork(net, idx);
      });
    }

    html += '<div style="margin-top:12px;text-align:center;">' +
      '<button class="btn-small btn-add" onclick="addWifiNetwork()">+ Add Network</button>' +
    '</div></div>';
    container.innerHTML = html;
  }

  async function loadWifiNetworks() {
    try {
      const resp = await fetch('/api/wifi');
      if (!resp.ok) throw new Error('Failed to load');
      wifiNetworks = await resp.json();
      renderWifiSection();
    } catch (e) {
      console.error('Wi-Fi load error:', e);
    }
  }

  function addWifiNetwork() {
    const container = document.getElementById('wifi-container');
    const card = container.querySelector('.card');
    const addBtn = card.querySelector('.btn-add').parentElement;
    // Prevent multiple unsaved new-network forms at once (idx -1 -> id "new")
    if (document.getElementById('wifi-new')) return;
    addBtn.insertAdjacentHTML('beforebegin', renderWifiNetwork({ssid:'',hasPassword:false,isLastConnected:false}, -1));
  }

  async function saveWifiNetwork(idx) {
    const id = idx === -1 ? 'new' : idx;
    const ssid = document.getElementById('wifi-ssid-' + id).value.trim();
    if (!ssid) {
      showMessage('SSID is required.', true);
      return;
    }

    const data = { ssid: ssid };
    // Only include password when the user actually typed something; omitting it
    // tells the server to preserve an existing password.
    const pass = document.getElementById('wifi-pass-' + id).value;
    if (pass) data.password = pass;
    if (idx >= 0) data.index = idx;

    try {
      const resp = await fetch('/api/wifi', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(data)
      });
      if (!resp.ok) throw new Error(await resp.text());
      showMessage('Wi-Fi network saved!', false);
      await loadWifiNetworks();
    } catch (e) {
      showMessage('Error: ' + e.message, true);
    }
  }

  async function deleteWifiNetwork(idx) {
    if (!await showWebConfirm('Delete this Wi-Fi network?')) return;
    try {
      const resp = await fetch('/api/wifi/delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({index: idx})
      });
      if (!resp.ok) throw new Error(await resp.text());
      showMessage('Wi-Fi network deleted', false);
      await loadWifiNetworks();
    } catch (e) {
      showMessage('Error: ' + e.message, true);
    }
  }

  // --- OPDS Server Management ---
  // Dynamically renders an editable list of OPDS servers, communicating with the
  // /api/opds REST endpoints. Password fields are never pre-filled for security;
  // the "(unchanged)" placeholder indicates an existing password is preserved on save.
  let opdsServers = [];

  function opdsFolderMode(folder) {
    return !folder ? 'default' : folder === '/' ? 'root' : 'custom';
  }

  function updateOpdsDestinations() {
    document.querySelectorAll('[data-opds-folder]').forEach(function(server) {
      const id = server.dataset.opdsFolder;
      const mode = document.getElementById('opds-folder-mode-' + id).value;
      const input = document.getElementById('opds-folder-' + id);
      document.getElementById('opds-folder-path-' + id).hidden = mode !== 'custom';
      const path = mode === 'default' ? originalValues.opdsDownloadFolder || '/' :
        mode === 'root' ? '/' : input.value.trim();
      document.getElementById('opds-destination-' + id).textContent =
        'Downloads to: ' + (path || 'Enter folder path') + (mode === 'default' ? ' (default)' : '');
    });
  }

  async function saveOpdsDefaultFolder() {
    const input = document.getElementById('setting-opdsDownloadFolder');
    try {
      const resp = await fetch('/api/settings', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ opdsDownloadFolder: input.value })
      });
      if (!resp.ok) throw new Error(await resp.text());
      // Read the normalized saved path without replacing unsaved server forms.
      const settings = await fetch('/api/settings');
      if (!settings.ok) throw new Error('Folder saved, but settings could not be refreshed');
      const saved = (await settings.json()).find(function(s) { return s.key === 'opdsDownloadFolder'; });
      if (saved) {
        input.value = saved.value;
        originalValues.opdsDownloadFolder = saved.value;
        const setting = allSettings.find(function(s) { return s.key === 'opdsDownloadFolder'; });
        if (setting) setting.value = saved.value;
      }
      updateOpdsDestinations();
      markChanged();
      showMessage('Default download folder saved!', false);
    } catch (error) { showMessage('Error: ' + error.message, true); }
  }

  function renderOpdsServer(srv, idx) {
    const isNew = idx === -1;
    const id = isNew ? 'new' : idx;
    return '<div class="opds-server" id="opds-' + id + '" data-opds-folder="' + id + '">' +
      '<div class="setting-row">' +
        '<span class="setting-name">Server Name</span>' +
        '<span class="setting-control"><input type="text" id="opds-name-' + id + '" value="' + escapeHtml(srv.name || '') + '"></span>' +
      '</div>' +
      '<div class="setting-row">' +
        '<span class="setting-name">URL</span>' +
        '<span class="setting-control"><input type="text" id="opds-url-' + id + '" value="' + escapeHtml(srv.url || '') + '"></span>' +
      '</div>' +
      '<div class="setting-row">' +
        '<span class="setting-name">Username</span>' +
        '<span class="setting-control"><input type="text" id="opds-user-' + id + '" value="' + escapeHtml(srv.username || '') + '"></span>' +
      '</div>' +
      '<div class="setting-row">' +
        '<span class="setting-name">Password</span>' +
        '<span class="setting-control"><input type="password" id="opds-pass-' + id + '" placeholder="' + (srv.hasPassword ? '(unchanged)' : '') + '"></span>' +
      '</div>' +
      '<div class="setting-row">' +
        '<span class="setting-name">Filename</span>' +
        '<span class="setting-control"><select id="opds-filename-' + id + '">' +
          '<option value="author_title"' + ((srv.filenameFormat || 'author_title') === 'author_title' ? ' selected' : '') + '>Author - Title</option>' +
          '<option value="server_filename"' + (srv.filenameFormat === 'server_filename' ? ' selected' : '') + '>Server filename</option>' +
          '<option value="title_author"' + (srv.filenameFormat === 'title_author' ? ' selected' : '') + '>Title - Author</option>' +
        '</select></span>' +
      '</div>' +
      '<div class="setting-row">' +
        '<label class="setting-name" for="opds-folder-mode-' + id + '">Download Folder</label>' +
        '<span class="setting-control"><select id="opds-folder-mode-' + id + '" onchange="updateOpdsDestinations()">' +
          '<option value="default"' + (opdsFolderMode(srv.downloadFolder) === 'default' ? ' selected' : '') + '>Use default</option>' +
          '<option value="root"' + (opdsFolderMode(srv.downloadFolder) === 'root' ? ' selected' : '') + '>SD root</option>' +
          '<option value="custom"' + (opdsFolderMode(srv.downloadFolder) === 'custom' ? ' selected' : '') + '>Custom folder</option>' +
        '</select></span>' +
      '</div>' +
      '<div class="setting-row" id="opds-folder-path-' + id + '"' + (opdsFolderMode(srv.downloadFolder) !== 'custom' ? ' hidden' : '') + '>' +
        '<label class="setting-name" for="opds-folder-' + id + '">Folder path</label>' +
        '<span class="setting-control"><input type="text" id="opds-folder-' + id + '" maxlength="63" placeholder="/Books/Calibre" value="' + escapeHtml(srv.downloadFolder && srv.downloadFolder !== '/' ? srv.downloadFolder : '') + '" oninput="updateOpdsDestinations()"></span>' +
      '</div>' +
      '<p class="opds-destination" id="opds-destination-' + id + '" aria-live="polite"></p>' +
      '<div class="opds-actions">' +
        '<button class="btn-small btn-save-server" onclick="saveOpdsServer(' + idx + ')">Save</button>' +
        (isNew ? '' : '<button class="btn-small btn-delete" onclick="deleteOpdsServer(' + idx + ')">Delete</button>') +
      '</div>' +
    '</div>';
  }

  function renderOpdsSection() {
    const container = document.getElementById('opds-container');
    let html = '<div class="card"><h2>OPDS Servers</h2>' +
      '<div class="setting-row"><label class="setting-name" for="setting-opdsDownloadFolder">Default download folder</label>' +
      '<span class="setting-control"><input type="text" id="setting-opdsDownloadFolder" maxlength="63" value="' + escapeHtml(originalValues.opdsDownloadFolder || '') + '"></span></div>' +
      '<div class="opds-actions"><button class="btn-small" onclick="saveOpdsDefaultFolder()">Save default</button></div>' +
      '<p class="opds-folder-hint">Used by servers set to Use default. Blank means SD root.</p>';

    if (opdsServers.length === 0) {
      html += '<p style="color:var(--label-color);text-align:center;">No OPDS servers configured</p>';
    } else {
      opdsServers.forEach(function(srv, idx) {
        html += renderOpdsServer(srv, idx);
      });
    }

    html += '<div style="margin-top:12px;text-align:center;">' +
      '<button class="btn-small btn-add" onclick="addOpdsServer()">+ Add Server</button>' +
    '</div></div>';
    container.innerHTML = html;
    updateOpdsDestinations();
  }

  async function loadOpdsServers() {
    try {
      const resp = await fetch('/api/opds');
      if (!resp.ok) throw new Error('Failed to load');
      opdsServers = await resp.json();
      renderOpdsSection();
    } catch (e) {
      console.error('OPDS load error:', e);
    }
  }

  function addOpdsServer() {
    const container = document.getElementById('opds-container');
    const card = container.querySelector('.card');
    const addBtn = card.querySelector('.btn-add').parentElement;
    // Prevent multiple unsaved new-server forms at once (idx -1 → id "new")
    if (document.getElementById('opds-new')) return;
    addBtn.insertAdjacentHTML('beforebegin', renderOpdsServer({name:'',url:'',username:'',hasPassword:false,filenameFormat:'author_title'}, -1));
    updateOpdsDestinations();
  }

  async function saveOpdsServer(idx) {
    const id = idx === -1 ? 'new' : idx;
    const data = {
      name: document.getElementById('opds-name-' + id).value,
      url: document.getElementById('opds-url-' + id).value,
      username: document.getElementById('opds-user-' + id).value,
      filenameFormat: document.getElementById('opds-filename-' + id).value,
    };
    const mode = document.getElementById('opds-folder-mode-' + id).value;
    const folder = document.getElementById('opds-folder-' + id).value.trim();
    if (mode === 'custom' && !folder) {
      showMessage('Enter a folder path, or choose Use default.', true);
      return;
    }
    data.downloadFolder = mode === 'default' ? '' : mode === 'root' ? '/' : folder;
    // Only include password in payload when the user actually typed something;
    // omitting it tells the server to keep the existing password.
    const pass = document.getElementById('opds-pass-' + id).value;
    if (pass) data.password = pass;
    if (idx >= 0) data.index = idx;

    try {
      const resp = await fetch('/api/opds', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(data)
      });
      if (!resp.ok) throw new Error(await resp.text());
      showMessage('OPDS server saved!', false);
      await loadOpdsServers();
    } catch (e) {
      showMessage('Error: ' + e.message, true);
    }
  }

  async function deleteOpdsServer(idx) {
    if (!await showWebConfirm('Delete this OPDS server?')) return;
    try {
      const resp = await fetch('/api/opds/delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({index: idx})
      });
      if (!resp.ok) throw new Error(await resp.text());
      showMessage('OPDS server deleted', false);
      await loadOpdsServers();
    } catch (e) {
      showMessage('Error: ' + e.message, true);
    }
  }

  // Sequential, not concurrent: the device's web server handles one client
  // connection at a time, and three simultaneous fetches on page load can
  // stall long enough to delay or interrupt a response.
  (async () => {
    await loadSettings();
    await loadStatusBars();
    await loadWifiNetworks();
    await loadOpdsServers();
  })();

loadPlugins("settings");
