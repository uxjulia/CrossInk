// Keep progress/results visible until dismissed or replaced, even during slow uploads.
function showFontStatus(status, text) {
  document.getElementById('fontFeedback').hidden = false;
  status.textContent = text;
}

let ttfSupported = false;
function formatSize(bytes) {
      if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
      if (bytes >= 1024) return (bytes / 1024).toFixed(0) + ' KB';
      return bytes + ' B';
    }

    async function loadFonts() {
      const el = document.getElementById('families');
      try {
        const res = await fetch('/api/fonts');
        const data = await res.json();
        ttfSupported = data.ttfSupported === true;
        document.getElementById('formatHelp').textContent = ttfSupported
          ? 'Upload .cpfont packs or static .ttf/.otf fonts (up to 2 MiB per file). Variable fonts and collections are unsupported.'
          : 'Upload .cpfont font packs. Scalable .ttf/.otf fonts require a supported PSRAM device.';
        // Build rows with DOM APIs and textContent so on-device family names
        // (which can contain arbitrary characters) cannot break markup or
        // execute script via innerHTML / inline onclick interpolation.
        el.replaceChildren();
        if (!data.families || data.families.length === 0) {
          const p = document.createElement('p');
          p.className = 'empty';
          p.textContent = 'No fonts installed';
          el.appendChild(p);
          return;
        }
        for (const f of data.families) {
          const row = document.createElement('div');
          row.className = 'family';

          const info = document.createElement('div');
          info.className = 'family-info';
          const h3 = document.createElement('h3');
          h3.textContent = f.name;
          info.appendChild(h3);
          const meta = document.createElement('span');
          meta.className = 'family-meta';
          const sizes = (f.sizes || []).join(', ');
          const filesSizes = (f.files || []).map(fi => formatSize(fi.size)).join(' + ');
          meta.textContent = sizes + 'pt · ' + filesSizes;
          info.appendChild(meta);

          const btn = document.createElement('button');
          btn.className = 'btn btn-danger';
          btn.textContent = 'Delete';
          // Capture name in the closure rather than interpolating into onclick.
          const familyName = f.name;
          btn.addEventListener('click', () => deleteFamily(familyName));

          row.appendChild(info);
          row.appendChild(btn);
          el.appendChild(row);
        }
      } catch (e) {
        el.replaceChildren();
        const p = document.createElement('p');
        p.className = 'empty';
        p.textContent = 'Failed to load font list';
        el.appendChild(p);
      }
    }

    async function deleteFamily(name) {
      if (!await showWebConfirm('Delete font family "' + name + '"?')) return;
      const status = document.getElementById('status');
      status.className = '';
      showFontStatus(status, 'Deleting ' + name + '...');
      try {
        const res = await fetch('/api/fonts/delete', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({family: name})
        });
        if (res.ok) {
          status.className = 'status-ok';
          showFontStatus(status, 'Deleted "' + name + '".');
        } else {
          status.className = 'status-err';
          showFontStatus(status, 'Failed to delete "' + name + '".');
        }
      } catch (err) {
        status.className = 'status-err';
        showFontStatus(status, 'Delete error: ' + err.message);
      }
      await loadFonts();
    }

    // Derive family name from a .cpfont filename: take everything before the
    // last '-' or '_' (that separator precedes the size suffix, e.g. Bookerly_12.cpfont).
    function familyFromFilename(name) {
      if (/\.(ttf|otf)$/i.test(name)) return name.replace(/\.(ttf|otf)$/i, '').replace(/[-_ ]?(Bold[-_ ]?Italic|Regular|Medium|Bold|Italic)$/i, '');
      const stem = name.replace(/\.cpfont$/i, '');
      const cut = Math.max(stem.lastIndexOf('-'), stem.lastIndexOf('_'));
      return cut > 0 ? stem.slice(0, cut) : stem;
    }

    // Sanitize to match firmware's [A-Za-z0-9_-]+ pattern.
    function sanitizeFamily(raw) {
      return raw.replace(/[^A-Za-z0-9_-]/g, '_');
    }

    function cpfontFilesOnly(fileList) {
      return Array.from(fileList).filter(f => (/\.cpfont$/i.test(f.name) || (ttfSupported && /\.(ttf|otf)$/i.test(f.name))));
    }

    document.getElementById('fontFiles').addEventListener('change', function() {
      const info = document.getElementById('pickedInfo');
      const files = cpfontFilesOnly(this.files);
      if (files.length === 0) {
        info.textContent = 'No supported font files found in the selected folder.';
        return;
      }
      const family = sanitizeFamily(familyFromFilename(files[0].name));
      info.textContent = files.length + ' file' + (files.length === 1 ? '' : 's') +
        ' → family "' + family + '"';
    });

    document.getElementById('uploadForm').addEventListener('submit', async function(e) {
      e.preventDefault();
      const status = document.getElementById('status');
      const files = cpfontFilesOnly(document.getElementById('fontFiles').files);
      if (files.length === 0) {
        status.className = 'status-err';
        showFontStatus(status, 'No supported font files selected.');
        return;
      }

      // A directory picker may include files from multiple family subfolders.
      // Reject that up front — otherwise files[0]'s family is silently reused
      // for every upload, corrupting the install layout.
      const families = [...new Set(files.map(f => sanitizeFamily(familyFromFilename(f.name))))];
      if (families.length !== 1) {
        status.className = 'status-err';
        showFontStatus(status, 'Please select files from a single font family.');
        return;
      }
      const family = families[0];

      status.className = '';

      let uploaded = 0;
      for (const file of files) {
        showFontStatus(status, 'Uploading ' + (uploaded + 1) + '/' + files.length + ': ' + file.name);
        const formData = new FormData();
        formData.append('family', family);
        formData.append('file', file, file.name);
        try {
          const res = await fetch('/api/fonts/upload', { method: 'POST', body: formData });
          const data = await res.json();
          if (!data.ok) {
            status.className = 'status-err';
            showFontStatus(status, 'Failed on ' + file.name + ': ' + (data.error || 'unknown error'));
            await loadFonts();
            return;
          }
        } catch (err) {
          status.className = 'status-err';
          showFontStatus(status, 'Upload error on ' + file.name + ': ' + err.message);
          await loadFonts();
          return;
        }
        uploaded++;
      }

      status.className = 'status-ok';
      showFontStatus(status, 'Uploaded ' + uploaded + ' file' + (uploaded === 1 ? '' : 's') +
        ' to family "' + family + '".');
      await loadFonts();
    });

    loadFonts();
