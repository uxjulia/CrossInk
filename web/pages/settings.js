let allSettings = [];
  let originalValues = {};
  let preserveQuickResumeTimeoutOn = false;
  let quickResumeTimeoutAutoEnabled = false;
  const SLEEP_SCREEN_MODE = {
    QUICK_RESUME: 6
  };
  const POWER_BUTTON_FOOTNOTES_DISPLAY_INDEX = 15;
  const LONG_PRESS_MENU_FOOTNOTES_DISPLAY_INDEX = 14;

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
    msg.textContent = text;
    msg.className = 'message ' + (isError ? 'error' : 'success');
    msg.style.display = 'block';
    setTimeout(function() { msg.style.display = 'none'; }, 4000);
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
      return '<input type="number" id="' + id + '" value="' + setting.value + '"' +
        ' min="' + setting.min + '" max="' + setting.max + '" step="' + setting.step + '"' +
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
      return parseInt(el.value, 10);
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
    const shortPwrBtnVal = getControlValue('shortPwrBtn');
    const longPwrBtnVal = getControlValue('longPwrBtn');
    const longPressMenuActionVal = getControlValue('longPressMenuAction');
    const pwrBtnFootnoteBackRow = document.getElementById('row-setting-pwrBtnFootnoteBack');
    if (pwrBtnFootnoteBackRow) {
      if (shortPwrBtnVal === POWER_BUTTON_FOOTNOTES_DISPLAY_INDEX ||
          longPwrBtnVal === POWER_BUTTON_FOOTNOTES_DISPLAY_INDEX ||
          longPressMenuActionVal === LONG_PRESS_MENU_FOOTNOTES_DISPLAY_INDEX) {
        pwrBtnFootnoteBackRow.style.display = '';
      } else {
        pwrBtnFootnoteBackRow.style.display = 'none';
      }
    }
  }

  function handleSettingChanged(key) {
    syncQuickResumeTimeoutForSleepScreen(key === 'sleepScreen', key === 'quickResumeSleepScreen');
    if (key === 'shortPwrBtn' || key === 'longPwrBtn' || key === 'longPressMenuAction') {
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
          html += '<div class="setting-row" id="row-setting-' + s.key + '">' +
            '<span class="setting-name">' + escapeHtml(s.name) + '</span>' +
            '<span class="setting-control">' + renderControl(s) + '</span>' +
            '</div>';
        });
        html += '</div>';
      }

      container.innerHTML = html;
      updateSettingsVisibility();
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

      showMessage('Settings saved successfully!', false);
    } catch (e) {
      console.error(e);
      showMessage('Error: ' + e.message, true);
    }

    btn.textContent = 'Save Settings';
  }

  loadSettings();

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
    if (!confirm('Delete this Wi-Fi network?')) return;
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

  function renderOpdsServer(srv, idx) {
    const isNew = idx === -1;
    const id = isNew ? 'new' : idx;
    return '<div class="opds-server" id="opds-' + id + '">' +
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
          '<option value="title_author"' + (srv.filenameFormat === 'title_author' ? ' selected' : '') + '>Title - Author</option>' +
        '</select></span>' +
      '</div>' +
      '<div class="opds-actions">' +
        '<button class="btn-small btn-save-server" onclick="saveOpdsServer(' + idx + ')">Save</button>' +
        (isNew ? '' : '<button class="btn-small btn-delete" onclick="deleteOpdsServer(' + idx + ')">Delete</button>') +
      '</div>' +
    '</div>';
  }

  function renderOpdsSection() {
    const container = document.getElementById('opds-container');
    let html = '<div class="card"><h2>OPDS Servers</h2>';

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
  }

  async function saveOpdsServer(idx) {
    const id = idx === -1 ? 'new' : idx;
    const data = {
      name: document.getElementById('opds-name-' + id).value,
      url: document.getElementById('opds-url-' + id).value,
      username: document.getElementById('opds-user-' + id).value,
      filenameFormat: document.getElementById('opds-filename-' + id).value,
    };
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
    if (!confirm('Delete this OPDS server?')) return;
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

  loadWifiNetworks();
  loadOpdsServers();
