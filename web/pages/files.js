// get current path from query parameter
const currentPath = decodeURIComponent(new URLSearchParams(window.location.search).get("path") || "/");

if (currentPath !== "/") {
  const leaf = currentPath.split("/").filter(Boolean).pop();
  if (leaf) document.title = leaf + " - Files - CrossInk Reader";
}

// Network status monitoring
let isNetworkOnline = navigator.onLine;

// Add network status listeners
window.addEventListener("online", () => {
  console.log("[Network] Online event fired");
  isNetworkOnline = true;
  showNotification("Network connection restored", "success");
});

window.addEventListener("offline", () => {
  console.log("[Network] Offline event fired");
  isNetworkOnline = false;
  showNotification("Network connection lost", "warning");
});

// Initialize network status
console.log("[Network] Initial status:", isNetworkOnline ? "online" : "offline");

function escapeHtml(unsafe) {
  return unsafe
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function showNotification(message, type = "info") {
  // Create notification element if it doesn't exist
  let notification = document.getElementById("notification");
  if (!notification) {
    notification = document.createElement("div");
    notification.id = "notification";
    notification.style.cssText = `
        position: fixed;
        top: 20px;
        right: 20px;
        padding: 12px 20px;
        border-radius: 4px;
        color: white;
        font-weight: 500;
        z-index: 10000;
        max-width: 300px;
        box-shadow: 0 2px 8px rgba(0,0,0,0.2);
        transition: all 0.3s ease;
      `;
    document.body.appendChild(notification);
  }

  // Set styles based on type
  const styles = {
    success: "background-color: #27ae60;",
    error: "background-color: #e74c3c;",
    warning: "background-color: #f39c12;",
    info: "background-color: #3498db;",
  };
  notification.style.cssText += styles[type] || styles["info"];
  notification.textContent = message;

  // Show notification
  notification.style.opacity = "1";
  notification.style.transform = "translateX(0)";

  // Auto-hide after 5 seconds
  setTimeout(() => {
    notification.style.opacity = "0";
    notification.style.transform = "translateX(100%)";
    setTimeout(() => {
      if (notification.parentNode) {
        notification.parentNode.removeChild(notification);
      }
    }, 300);
  }, 5000);
}

function formatFileSize(bytes) {
  if (bytes === 0) return "0 B";
  const k = 1024;
  const sizes = ["B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)).toLocaleString() + " " + sizes[i];
}

async function hydrate() {
  // Fetch CrossInk version
  fetchVersion();

  // Close modals when clicking overlay - call proper cleanup functions
  document.querySelectorAll(".modal-overlay").forEach(function (overlay) {
    overlay.addEventListener("click", function (e) {
      if (e.target === overlay) {
        // Call the appropriate close function for each modal to ensure cleanup
        if (overlay.id === "uploadModal") return closeUploadModal();
        if (overlay.id === "folderModal") return closeFolderModal();
        if (overlay.id === "deleteModal") return closeDeleteModal();
        if (overlay.id === "renameModal") return closeRenameModal();
        if (overlay.id === "moveModal") return closeMoveModal();
        if (overlay.id === "imagePreviewModal") return closeImagePreview();
        overlay.classList.remove("open");
      }
    });
  });

  const breadcrumbs = document.getElementById("directory-breadcrumbs");
  const fileTable = document.getElementById("file-table");

  let breadcrumbContent = '<span class="sep">/</span>';
  if (currentPath === "/") {
    breadcrumbContent += '<span class="current">🏠</span>';
  } else {
    breadcrumbContent += '<a href="/files">🏠</a>';
    const pathSegments = currentPath.split("/");
    pathSegments.slice(1, pathSegments.length - 1).forEach(function (segment, index) {
      breadcrumbContent +=
        '<span class="sep">/</span><a href="/files?path=' +
        encodeURIComponent(pathSegments.slice(0, index + 2).join("/")) +
        '">' +
        escapeHtml(segment) +
        "</a>";
    });
    breadcrumbContent += '<span class="sep">/</span>';
    breadcrumbContent += '<span class="current">' + escapeHtml(pathSegments[pathSegments.length - 1]) + "</span>";
  }
  breadcrumbs.innerHTML = breadcrumbContent;

  let files = [];
  try {
    const response = await fetch("/api/files?path=" + encodeURIComponent(currentPath) + "&_=" + Date.now());
    if (!response.ok) {
      throw new Error("Failed to load files: " + response.status + " " + response.statusText);
    }
    files = await response.json();
  } catch (e) {
    console.error(e);
    fileTable.innerHTML = '<div class="no-files">An error occurred while loading the files</div>';
    return;
  }

  let folderCount = 0;
  let totalSize = 0;
  files.forEach((file) => {
    if (file.isDirectory) folderCount++;
    totalSize += file.size;
  });

  const fileCount = files.length - folderCount;
  const folderLabel = folderCount === 1 ? "folder" : "folders";
  const fileLabel = fileCount === 1 ? "file" : "files";
  document.getElementById("folder-summary").innerHTML =
    `${folderCount} ${folderLabel}, ${fileCount} ${fileLabel}, ${formatFileSize(totalSize)}`;

  if (files.length === 0) {
    fileTable.innerHTML = '<div class="no-files">This folder is empty</div>';
  } else {
    let fileTableContent = '<table class="file-table">';

    // Add select-all checkbox column
    fileTableContent +=
      '<tr><th style="width:40px"><input type="checkbox" id="selectAllCheckbox" onchange="toggleSelectAll(this)"></th><th>Name</th><th>Type</th><th>Size</th><th class="actions-col">Actions</th></tr>';

    const sortedFiles = files.sort((a, b) => {
      // Directories first, then epub files, then other files, alphabetically within each group
      if (a.isDirectory && !b.isDirectory) return -1;
      if (!a.isDirectory && b.isDirectory) return 1;
      if (a.isEpub && !b.isEpub) return -1;
      if (!a.isEpub && b.isEpub) return 1;
      return a.name.localeCompare(b.name);
    });

    sortedFiles.forEach((file) => {
      if (file.isDirectory) {
        let folderPath = currentPath;
        if (!folderPath.endsWith("/")) folderPath += "/";
        folderPath += file.name;

        // Checkbox cell + folder row
        fileTableContent += `<tr class="folder-row">`;
        fileTableContent += `<td><input type="checkbox" class="select-item" data-path="${encodeURIComponent(folderPath)}" data-name="${escapeHtml(file.name)}" data-type="folder"></td>`;
        fileTableContent += `<td><span class="file-icon">📁</span><a href="/files?path=${encodeURIComponent(folderPath)}" class="folder-link">${escapeHtml(file.name)}</a></td>`;
        fileTableContent += '<td><span class="folder-badge">FOLDER</span></td>';
        fileTableContent += "<td>-</td>";
        fileTableContent += `<td class="actions-col"><div class="action-icon-group"><button class="delete-btn file-action-btn" data-action="delete" data-name="${escapeHtml(file.name)}" data-path="${encodeURIComponent(folderPath)}" data-is-folder="true" title="Delete folder">🗑️</button></div></td>`;
        fileTableContent += "</tr>";
      } else {
        let filePath = currentPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += file.name;

        // Checkbox cell + file row
        fileTableContent += `<tr class="${file.isEpub ? "epub-file" : ""}">`;
        fileTableContent += `<td><input type="checkbox" class="select-item" data-path="${encodeURIComponent(filePath)}" data-name="${escapeHtml(file.name)}" data-type="file"></td>`;
        const fileIsImage = isImageFile(file.name);
        fileTableContent += `<td><span class="file-icon">${file.isEpub ? "📗" : fileIsImage ? "🖼️" : "📄"}</span>`;
        if (fileIsImage) {
          fileTableContent += `<a href="${downloadUrl(filePath)}" class="file-link image-preview-link">${escapeHtml(file.name)}</a>`;
        } else {
          fileTableContent += `<a rel="noopener noreferrer" target="_blank" href="${downloadUrl(filePath)}" class="file-link">${escapeHtml(file.name)}</a>`;
        }
        fileTableContent += "</td>";
        fileTableContent += file.isEpub
          ? '<td><span class="epub-badge">EPUB</span></td>'
          : `<td>${escapeHtml(file.name.split(".").pop().toUpperCase())}</td>`;
        fileTableContent += `<td>${formatFileSize(file.size)}</td>`;
        fileTableContent += `<td class="actions-col"><div class="action-icon-group">`;
        fileTableContent += `<button class="move-btn file-action-btn" data-action="move" data-name="${escapeHtml(file.name)}" data-path="${encodeURIComponent(filePath)}" title="Move file">📂</button>`;
        fileTableContent += `<button class="rename-btn file-action-btn" data-action="rename" data-name="${escapeHtml(file.name)}" data-path="${encodeURIComponent(filePath)}" title="Rename file">✏️</button>`;
        fileTableContent += `<button class="delete-btn file-action-btn" data-action="delete" data-name="${escapeHtml(file.name)}" data-path="${encodeURIComponent(filePath)}" data-is-folder="false" title="Delete file">🗑️</button>`;
        fileTableContent += `</div></td>`;
        fileTableContent += "</tr>";
      }
    });

    fileTableContent += "</table>";
    fileTable.innerHTML = fileTableContent;
    fileTable.querySelectorAll(".file-action-btn").forEach((button) => {
      button.addEventListener("click", handleFileActionClick);
    });
  }
}

function isImageFile(name) {
  return /\.(png|jpe?g|bmp|gif|webp)$/i.test(name);
}

function downloadUrl(filePath) {
  return `/download?path=${encodeURIComponent(filePath)}`;
}

function openImagePreview(url, name) {
  const img = document.getElementById("imagePreviewImg");
  document.getElementById("imagePreviewName").textContent = name;
  img.src = url;
  img.alt = name;
  document.getElementById("imagePreviewDownload").href = url;
  document.getElementById("imagePreviewModal").classList.add("open");
}

function closeImagePreview() {
  document.getElementById("imagePreviewModal").classList.remove("open");
  document.getElementById("imagePreviewImg").src = "";
}

function handleFileActionClick(event) {
  const button = event.currentTarget;
  const name = button.dataset.name || "";
  const path = decodeURIComponent(button.dataset.path || "");

  if (button.dataset.action === "move") {
    openMoveModal(name, path);
  } else if (button.dataset.action === "rename") {
    openRenameModal(name, path);
  } else if (button.dataset.action === "delete") {
    openDeleteModal(name, path, button.dataset.isFolder === "true");
  }
}

// Modal functions
function openUploadModal() {
  imageStates = {};
  restoreUploadSettingsFromStorage();

  // Hide convert options when opening modal (no files selected initially)
  const convertOptions = document.getElementById("convertOptions");
  if (convertOptions) {
    convertOptions.style.display = "none";
  }

  // Hide log section from previous session
  const logSection = document.getElementById("log-section");
  if (logSection) logSection.classList.remove("visible");
  const logContainer = document.getElementById("log-container");
  if (logContainer) logContainer.innerHTML = "";

  document.getElementById("uploadPathDisplay").textContent = currentPath === "/" ? "/ 🏠" : currentPath;
  document.getElementById("uploadModal").classList.add("open");
}

function handleCancelUploadModal() {
  if (!isUploadInProgress) {
    // No process running: close the modal normally
    closeUploadModal();
    return;
  }
  // Process running: stop it, keep modal open for retry
  operationCancelled = true;
  if (currentUploadWs) {
    currentUploadWs.close();
    currentUploadWs = null;
  }
  if (currentUploadXhr) {
    currentUploadXhr.abort();
    currentUploadXhr = null;
  }
  // isUploadInProgress and UI are restored by restoreAfterCancel() from the async handlers
}

function restoreAfterCancel() {
  operationCancelled = false;
  isUploadInProgress = false;
  document.getElementById("uploadModalClose").classList.remove("disabled");
  document.getElementById("fileInput").disabled = false;
  const progressFill = document.getElementById("progress-fill");
  const progressText = document.getElementById("progress-text");
  progressFill.style.width = "0%";
  progressFill.style.backgroundColor = "#e74c3c";
  progressText.style.color = "#e74c3c";
  progressText.textContent = "Upload cancelled by User!";
  // Re-enable the action button (uploadBtn is always visible at this point;
  // its text is already "Upload" or "Optimize & Upload" depending on checkbox state)
  document.getElementById("uploadBtn").disabled = false;
}

function closeUploadModal() {
  // Prevent closing during upload/conversion
  if (isUploadInProgress) {
    return;
  }
  document.getElementById("uploadModal").classList.remove("open");
  const fileInput = document.getElementById("fileInput");
  fileInput.value = "";
  fileInput.classList.remove("has-files");
  fileInput.disabled = false;
  const uploadBtn = document.getElementById("uploadBtn");
  uploadBtn.disabled = true;
  uploadBtn.style.display = "block";
  uploadBtn.textContent = "Upload";
  uploadBtn.classList.remove("optimize");
  document.getElementById("startConversionBtn").style.display = "none";
  document.getElementById("progress-container").style.display = "none";
  document.getElementById("progress-fill").style.width = "0%";
  document.getElementById("progress-fill").style.backgroundColor = "#27ae60";
  const convertOptions = document.getElementById("convertOptions");
  if (convertOptions) convertOptions.style.display = "none";
  document.getElementById("convertInfo").style.display = "none";
  document.getElementById("convertWarning").style.display = "none";
  // Clear image picker cache and reset layout
  epubImagesCache = [];
  imageStates = {};
  document.getElementById("imagePickerSection").style.display = "none";
  const imageGrid = document.getElementById("imageGrid");
  if (imageGrid) imageGrid.innerHTML = "";
  document.querySelector("#uploadModal .modal").classList.remove("picker-mode");
  document.getElementById("pickerColumns").classList.remove("picker-active");
  // Hide log section
  if (logSection) logSection.classList.remove("visible");
  // Reset advanced options
  const advancedSettingsContent = document.getElementById("advancedSettingsContent");
  if (advancedSettingsContent) advancedSettingsContent.classList.remove("visible");
  document.getElementById("advancedOptionsArrow").classList.remove("expanded");
  const advancedOptionsToggle = document.getElementById("advancedOptionsToggle");
  if (advancedOptionsToggle) {
    advancedOptionsToggle.style.opacity = "0.5";
    advancedOptionsToggle.style.pointerEvents = "none";
  }
  applyUploadSettings();
}

function updateBatchModeUI(isBatch) {
  const rotationRow = document.getElementById("rotationSettingRow");
  const overlapRow = document.getElementById("overlapSettingRow");
  if (rotationRow) rotationRow.style.display = isBatch ? "none" : "";
  if (overlapRow) overlapRow.style.display = isBatch ? "none" : "";
}

function toggleConvertOptions() {
  const checked = document.getElementById("convertBeforeUpload").checked;
  const uploadBtn = document.getElementById("uploadBtn");
  document.getElementById("convertWarning").style.display = checked ? "block" : "none";
  document.getElementById("convertInfo").style.display = checked ? "block" : "none";
  // Update button text and style
  if (checked) {
    uploadBtn.textContent = "Optimize & Upload";
    uploadBtn.classList.add("optimize");
  } else {
    uploadBtn.textContent = "Upload";
    uploadBtn.classList.remove("optimize");
    // Clear image picker when unchecking
    clearImagePicker();
  }
  // Reset advanced options when toggling off
  if (!checked) {
    const advancedSettingsContent = document.getElementById("advancedSettingsContent");
    if (advancedSettingsContent) advancedSettingsContent.classList.remove("visible");
    document.getElementById("advancedOptionsArrow").classList.remove("expanded");
  }
  // Disable/enable advanced mode toggle
  const advancedOptionsToggle = document.getElementById("advancedOptionsToggle");
  if (advancedOptionsToggle) {
    advancedOptionsToggle.style.opacity = checked ? "1" : "0.5";
    advancedOptionsToggle.style.pointerEvents = checked ? "auto" : "none";
  }
  updateUploadSettingsPersistence();
}

function toggleAdvancedOptions() {
  // Check if advanced options toggle is enabled (optimize EPUB must be checked)
  const convertEnabled = document.getElementById("convertBeforeUpload").checked;
  if (!convertEnabled) {
    return; // Don't toggle if optimization is disabled
  }

  const content = document.getElementById("advancedSettingsContent");
  const arrow = document.getElementById("advancedOptionsArrow");
  const isExpanding = !content.classList.contains("visible");

  content.classList.toggle("visible");
  arrow.classList.toggle("expanded");

  // When expanding, show image picker if an EPUB is selected
  if (isExpanding) {
    const fileInput = document.getElementById("fileInput");
    const files = fileInput.files;
    if (files.length === 1 && files[0].name.toLowerCase().endsWith(".epub")) {
      showImagePicker(files[0]).catch((err) => console.error("Image picker error:", err));
    }
  } else {
    // When collapsing, hide the picker and startConversionBtn
    document.getElementById("imagePickerSection").style.display = "none";
    document.getElementById("startConversionBtn").style.display = "none";
    document.getElementById("uploadBtn").style.display = "block";
    document.getElementById("uploadBtn").disabled = false;
    document.querySelector("#uploadModal .modal").classList.remove("picker-mode");
    document.getElementById("pickerColumns").classList.remove("picker-active");
  }
}

function setQualityPreset(value) {
  document.getElementById("qualitySlider").value = value;
  document.getElementById("qualityInput").value = value;
  document.querySelectorAll(".quality-preset").forEach((btn) => {
    btn.classList.toggle("active", parseInt(btn.dataset.value, 10) === value);
  });
  updateQualitySettings();
}

function updateQualitySettings() {
  const quality = document.getElementById("qualitySlider").value;
  // Check if grayscaleToggle exists (may be hidden for compatibility with other devices)
  const grayscaleToggle = document.getElementById("grayscaleToggle");
  const grayscale = grayscaleToggle ? grayscaleToggle.checked : true; // Default to true for e-ink

  // Update displays (if element exists)
  const qualityDisplay = document.getElementById("qualityDisplaySimple");
  if (qualityDisplay) {
    qualityDisplay.textContent = "📦 " + quality + "% JPEG";
  }

  // Update converter variables (used by processImage and applyGrayscale)
  JPEG_QUALITY = parseInt(quality, 10);
  ENABLE_GRAYSCALE = true; // Always grayscale for e-ink
  updateUploadSettingsPersistence();
}

function setHandedness(value) {
  HANDEDNESS = value;
  // Update UI
  document.getElementById("rotationCW").classList.remove("active");
  document.getElementById("rotationCCW").classList.remove("active");
  document.getElementById(value === "right" ? "rotationCW" : "rotationCCW").classList.add("active");
  // Re-render grid to update rotation arrows
  if (document.getElementById("imagePickerSection").style.display !== "none") {
    renderImageGrid();
  }
  updateUploadSettingsPersistence();
}

function setOverlap(value) {
  OVERLAP_PERCENT = value;
  // Update UI
  document.querySelectorAll(".overlap-btn").forEach((btn) => {
    btn.classList.toggle("active", parseInt(btn.dataset.value) === value);
  });
  updateUploadSettingsPersistence();
}

// ============================================================================
// Image Picker Functions
// ============================================================================

/**
 * Extract images from EPUB for preview
 * Returns array of {path, name, dataUrl, width, height}
 */
async function extractImagesForPreview(file) {
  const zip = await JSZip.loadAsync(file);
  const imageExtensions = [".png", ".gif", ".webp", ".bmp", ".jpg", ".jpeg", ".svg"];

  // Collect all image paths first
  const allImages = [];
  for (const [path, fileObj] of Object.entries(zip.files)) {
    if (fileObj.dir) continue;
    const ext = path.substring(path.lastIndexOf(".")).toLowerCase();
    if (imageExtensions.includes(ext)) {
      allImages.push(path);
    }
  }

  // Try to get images in reading order from OPF spine
  let orderedImages = [];
  let coverImagePath = null; // Track cover image
  try {
    // Find OPF file
    let opfPath = null;
    zip.forEach((p) => {
      if (p.toLowerCase().endsWith(".opf")) opfPath = p;
    });

    if (opfPath) {
      const opfContent = await zip.files[opfPath].async("string");
      const opfDir = opfPath.includes("/") ? opfPath.substring(0, opfPath.lastIndexOf("/")) : "";

      // Detect cover image from OPF
      let coverId = null;
      let m;
      // Try 1: properties="cover-image"
      if ((m = opfContent.match(/<item[^>]+id=["']([^"']+)["'][^>]+properties="[^"]*cover-image[^"]*"/)))
        coverId = m[1];
      if (!coverId && (m = opfContent.match(/<item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id=["']([^"']+)["']/)))
        coverId = m[1];
      // Try 2: meta name="cover" content="id"
      if (!coverId && (m = opfContent.match(/<meta\s+name=["']cover["']\s+content=["']([^"']+)["']/))) coverId = m[1];
      if (!coverId && (m = opfContent.match(/<meta\s+content=["']([^"']+)["']\s+name=["']cover["']/))) coverId = m[1];

      // Parse manifest to get id -> href mapping
      const manifest = {};
      const manifestRegex = /<item[^>]+id=["']([^"']+)["'][^>]+href=["']([^"']+)["'][^>]*>/gi;
      let match;
      while ((match = manifestRegex.exec(opfContent)) !== null) {
        const id = match[1];
        const href = match[2];
        const fullPath = opfDir ? opfDir + "/" + href : href;
        manifest[id] = fullPath;
        if (id === coverId) coverImagePath = fullPath;
      }
      // Also check reversed attribute order
      const manifestRegex2 = /<item[^>]+href=["']([^"']+)["'][^>]+id=["']([^"']+)["'][^>]*>/gi;
      while ((match = manifestRegex2.exec(opfContent)) !== null) {
        const href = match[1];
        const id = match[2];
        const fullPath = opfDir ? opfDir + "/" + href : href;
        manifest[id] = fullPath;
        if (id === coverId) coverImagePath = fullPath;
      }

      // Cover-page reconciliation — if the cover XHTML references a different
      // but byte-identical image, prefer the one actually displayed on the page.
      if (coverImagePath) {
        try {
          let coverXhtmlPath = null;
          const guideM =
            opfContent.match(/<(?:\w+:)?reference[^>]+type=["']cover["'][^>]+href=["']([^"']+)["']/i) ||
            opfContent.match(/<(?:\w+:)?reference[^>]+href=["']([^"']+)["'][^>]+type=["']cover["']/i);
          if (guideM) {
            coverXhtmlPath = opfDir ? opfDir + "/" + decodeHref(guideM[1]) : decodeHref(guideM[1]);
          }
          if (!coverXhtmlPath) {
            const spineM = opfContent.match(/<(?:\w+:)?itemref[^>]+idref=["']([^"']+)["']/i);
            if (spineM && manifest[spineM[1]]) coverXhtmlPath = manifest[spineM[1]];
          }
          if (coverXhtmlPath && zip.files[coverXhtmlPath]) {
            const coverXhtml = await zip.files[coverXhtmlPath].async("string");
            const imgM = coverXhtml.match(/(?:src|xlink:href)=["']([^"']+)["']/i);
            if (imgM) {
              const href = imgM[1];
              const xDir = coverXhtmlPath.includes("/")
                ? coverXhtmlPath.substring(0, coverXhtmlPath.lastIndexOf("/"))
                : "";
              let pageImgPath = href.startsWith("../")
                ? xDir.split("/").slice(0, -1).join("/") + "/" + href.substring(3)
                : href.startsWith("/")
                  ? href.substring(1)
                  : xDir
                    ? xDir + "/" + href
                    : href;
              pageImgPath = pageImgPath.replace(/\/+/g, "/");
              for (const realPath of allImages) {
                if (realPath === pageImgPath || realPath.endsWith("/" + href) || realPath.endsWith(href)) {
                  pageImgPath = realPath;
                  break;
                }
              }
              if (pageImgPath !== coverImagePath && allImages.includes(pageImgPath) && zip.files[pageImgPath]) {
                const coverData = await zip.files[coverImagePath].async("arraybuffer");
                const pageData = await zip.files[pageImgPath].async("arraybuffer");
                if (coverData.byteLength === pageData.byteLength) {
                  const a = new Uint8Array(coverData);
                  const b = new Uint8Array(pageData);
                  let identical = true;
                  for (let i = 0; i < a.length; i++) {
                    if (a[i] !== b[i]) {
                      identical = false;
                      break;
                    }
                  }
                  if (identical) coverImagePath = pageImgPath;
                }
              }
            }
          }
        } catch (e) {
          /* non-critical */
        }
      }

      // Parse spine to get reading order
      const spineOrder = [];
      const spineRegex = /<itemref[^>]+idref=["']([^"']+)["'][^>]*>/gi;
      while ((match = spineRegex.exec(opfContent)) !== null) {
        const idref = match[1];
        if (manifest[idref]) spineOrder.push(manifest[idref]);
      }

      // For each spine item (XHTML), extract images in order
      const seenImages = new Set();
      for (const xhtmlPath of spineOrder) {
        if (!zip.files[xhtmlPath]) continue;
        const xhtmlContent = await zip.files[xhtmlPath].async("string");
        const xhtmlDir = xhtmlPath.includes("/") ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf("/")) : "";

        // Find all image references
        const imgRegex = /(?:src|xlink:href)=["']([^"']+)["']/gi;
        while ((match = imgRegex.exec(xhtmlContent)) !== null) {
          let imgHref = match[1];
          // Skip non-image references
          if (!imageExtensions.some((ext) => imgHref.toLowerCase().endsWith(ext))) continue;

          // Resolve relative path
          let imgPath;
          if (imgHref.startsWith("../")) {
            // Go up from xhtmlDir
            const parts = xhtmlDir.split("/");
            parts.pop();
            imgPath = parts.join("/") + "/" + imgHref.substring(3);
          } else if (imgHref.startsWith("/")) {
            imgPath = imgHref.substring(1);
          } else {
            imgPath = xhtmlDir ? xhtmlDir + "/" + imgHref : imgHref;
          }
          // Normalize path
          imgPath = imgPath.replace(/\/+/g, "/");

          // Check if this is actually an image in our list
          for (const realPath of allImages) {
            if (realPath === imgPath || realPath.endsWith("/" + imgHref) || realPath.endsWith(imgHref)) {
              if (!seenImages.has(realPath)) {
                seenImages.add(realPath);
                orderedImages.push(realPath);
              }
              break;
            }
          }
        }
      }

      // Add any remaining images that weren't in XHTML files (e.g., unused images)
      for (const imgPath of allImages) {
        if (!seenImages.has(imgPath)) {
          orderedImages.push(imgPath);
        }
      }
    }
  } catch (e) {
    console.warn("Failed to parse reading order, using default:", e);
  }

  // Fallback to alphabetical if parsing failed
  if (orderedImages.length === 0) {
    orderedImages = [...allImages].sort();
  }

  // Load image data in order
  const images = [];
  for (const path of orderedImages) {
    const data = await zip.files[path].async("arraybuffer");
    const blob = new Blob([data], { type: imageMimeType(path) });
    const dataUrl = URL.createObjectURL(blob);

    // Get dimensions
    const dims = await getImageDimensions(data, path);

    // Check if this is the cover image
    const isCover = path === coverImagePath || (path.toLowerCase().includes("cover") && images.length === 0);

    // Check if this is a separator/ornament (skip cover check)
    const filename = path.split("/").pop();
    let isSeparator = false;
    if (!isCover) {
      try {
        isSeparator = await isSeparatorImage(dataUrl, dims.width, dims.height, filename);
      } catch (e) {
        console.warn("Separator check failed for", filename, e);
      }
    }

    // Tiny images (<200x200) are locked like separators
    const isTiny = dims.width < 200 && dims.height < 200;

    // Images that fit screen can only rotate, not split
    const fitsScreen = dims.width <= MAX_WIDTH && dims.height <= MAX_HEIGHT;

    // Split capability - no upscaling allowed
    // H-Split scales width to MAX_HEIGHT (long edge), so needs width >= MAX_HEIGHT
    // V-Split scales height to MAX_HEIGHT, so needs height >= MAX_HEIGHT
    const canHSplit = dims.width >= MAX_HEIGHT;
    const canVSplit = dims.height >= MAX_HEIGHT;

    images.push({
      path: path,
      name: filename,
      dataUrl: dataUrl,
      width: dims.width,
      height: dims.height,
      isCover: isCover,
      isSeparator: isSeparator || isTiny,
      fitsScreen: fitsScreen,
      canHSplit: canHSplit,
      canVSplit: canVSplit,
    });
  }

  return images;
}

/**
 * Get image dimensions from array buffer
 */
function getImageDimensions(data, filename = "") {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(new Blob([data], { type: imageMimeType(filename) }));
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      resolve({ width: img.width, height: img.height });
    };
    img.onerror = () => {
      URL.revokeObjectURL(url);
      resolve({ width: 0, height: 0 });
    };
    img.src = url;
  });
}

/**
 * Check if image is a separator/ornament
 * Criteria: small size AND (filename match OR symmetric OR extreme aspect ratio)
 */
async function isSeparatorImage(dataUrl, width, height, filename) {
  const MAX_DIMENSION = 150;
  const SYMMETRY_THRESHOLD = 0.85;

  // First check: must be small in at least one dimension
  const isSmall = height < MAX_DIMENSION || width < MAX_DIMENSION;
  if (!isSmall) return false;

  // Filename hints (instant match if small + named correctly)
  const separatorNames = ["separator", "divider", "ornament", "break", "flourish", "scene", "divid", "decor"];
  const lowerName = filename.toLowerCase();
  if (separatorNames.some((n) => lowerName.includes(n))) return true;

  // Extreme aspect ratio check (>10:1 or <1:10) - these are definitely separators/lines
  const aspectRatio = width / height;
  if (aspectRatio > 10 || aspectRatio < 0.1) return true;

  // Symmetry check (skip for very thin images - too few pixels)
  if (width < 10 || height < 10) return true; // Very small = separator

  try {
    const isSymmetric = await checkHorizontalSymmetry(dataUrl, width, height, SYMMETRY_THRESHOLD);
    return isSymmetric;
  } catch (e) {
    console.warn("Symmetry check failed:", e);
    return false;
  }
}

/**
 * Check horizontal symmetry by comparing left and right halves
 */
function checkHorizontalSymmetry(dataUrl, width, height, threshold) {
  return new Promise((resolve) => {
    const img = new Image();
    img.onload = () => {
      // Use small canvas for performance (max 100px wide)
      const scale = Math.min(1, 100 / width);
      const w = Math.max(2, Math.floor(width * scale)); // Minimum 2px
      const h = Math.max(1, Math.floor(height * scale)); // Minimum 1px

      const canvas = document.createElement("canvas");
      canvas.width = w;
      canvas.height = h;
      const ctx = canvas.getContext("2d");

      // Draw scaled image
      ctx.drawImage(img, 0, 0, w, h);
      const imageData = ctx.getImageData(0, 0, w, h);
      const pixels = imageData.data;

      // Compare left half with flipped right half
      const halfW = Math.floor(w / 2);
      let matchingPixels = 0;
      let totalPixels = 0;

      for (let y = 0; y < h; y++) {
        for (let x = 0; x < halfW; x++) {
          const leftIdx = (y * w + x) * 4;
          const rightIdx = (y * w + (w - 1 - x)) * 4;

          // Compare RGB (ignore alpha)
          const rDiff = Math.abs(pixels[leftIdx] - pixels[rightIdx]);
          const gDiff = Math.abs(pixels[leftIdx + 1] - pixels[rightIdx + 1]);
          const bDiff = Math.abs(pixels[leftIdx + 2] - pixels[rightIdx + 2]);

          // Allow some tolerance for JPEG artifacts (threshold of 30)
          if (rDiff < 30 && gDiff < 30 && bDiff < 30) {
            matchingPixels++;
          }
          totalPixels++;
        }
      }

      const symmetryScore = matchingPixels / totalPixels;
      resolve(symmetryScore >= threshold);
    };
    img.onerror = () => resolve(false);
    img.src = dataUrl;
  });
}

/**
 * Show image picker after EPUB file selection
 */
async function showImagePicker(file) {
  // Check if JSZip is available
  if (typeof JSZip === "undefined") {
    console.error("JSZip not loaded");
    alert("JSZip library not available. Conversion will proceed without image picker.");
    startConversionWithImageStates();
    return;
  }

  const pickerSection = document.getElementById("imagePickerSection");
  const imageGrid = document.getElementById("imageGrid");
  const countDisplay = document.getElementById("imagePickerCount");

  // Reset state
  imageStates = {};
  epubImagesCache = [];
  pendingConversionFile = file;

  // Extract images
  try {
    const images = await extractImagesForPreview(file);
    epubImagesCache = images;

    // Initialize all states to 0 (Normal)
    images.forEach((img) => {
      imageStates[img.path] = 0;
    });

    // Build UI
    renderImageGrid();

    // Count images - covers and separators are locked
    const coverCount = images.filter((img) => img.isCover).length;
    const separatorCount = images.filter((img) => img.isSeparator).length;
    const lockedCount = coverCount + separatorCount;
    const selectableCount = images.length - lockedCount;

    if (lockedCount > 0) {
      const lockedParts = [];
      if (coverCount > 0) lockedParts.push(`${coverCount} cover`);
      if (separatorCount > 0) lockedParts.push(`${separatorCount} separator${separatorCount !== 1 ? "s" : ""}`);
      countDisplay.textContent = `${images.length} images (${selectableCount} selectable, ${lockedParts.join(", ")})`;
    } else {
      countDisplay.textContent = `${images.length} image${images.length !== 1 ? "s" : ""} (all selectable)`;
    }

    // Show picker, hide upload button, show start conversion button
    pickerSection.style.display = "block";
    document.getElementById("uploadBtn").style.display = "none";
    document.getElementById("startConversionBtn").style.display = "block";
    // Enable two-column layout
    document.querySelector("#uploadModal .modal").classList.add("picker-mode");
    document.getElementById("pickerColumns").classList.add("picker-active");
  } catch (error) {
    console.error("Failed to extract images:", error);
    alert("Failed to preview images: " + error.message + "\n\nConversion will proceed normally.");
    // Fallback: start conversion directly
    startConversionWithImageStates();
  }
}

/**
 * Render the image grid with current states
 */
function renderImageGrid() {
  const imageGrid = document.getElementById("imageGrid");
  imageGrid.innerHTML = "";

  const stateLabels = ["", "H-Split", "V-Split", "Rotate"];
  const stateClasses = ["state-0", "state-1", "state-2", "state-3"];

  epubImagesCache.forEach((img) => {
    // Cover images and separators are always locked (no splitting/rotation)
    const isCover = img.isCover;
    const isSeparator = img.isSeparator;
    const state = imageStates[img.path] || 0;

    const item = document.createElement("div");

    if (isCover) {
      // Cover image - locked, cannot be split
      item.className = "image-item cover-locked";
      item.title = `${img.width}×${img.height} - Cover image (locked)`;
      item.innerHTML = `
          <span class="image-state-badge cover-badge">🔒</span>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
    } else if (isSeparator) {
      // Separator/ornament - locked, cannot be split
      item.className = "image-item separator-locked";
      item.title = `${img.width}×${img.height} - Separator (locked)`;
      item.innerHTML = `
          <span class="image-state-badge separator-badge">✦</span>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
    } else {
      // All other images are selectable - all modes allowed

      // Determine which overlay to show based on state
      const showRotation = state === 1 || state === 3; // H-Split or Rotate
      const showSplitLines = state === 1 || state === 2; // H-Split or V-Split
      const rotateClass = showRotation ? (HANDEDNESS === "right" ? "rotate-cw" : "rotate-ccw") : "";

      // Calculate actual number of parts for split preview
      let numParts = 1;
      if (showSplitLines) {
        let finalWidth;
        if (state === 1) {
          // H-Split: scale width to MAX_HEIGHT, rotate, then check width
          const scaledH = Math.round(img.height * (MAX_HEIGHT / img.width));
          finalWidth = scaledH; // After rotation, height becomes width
        } else {
          // V-Split: scale height to MAX_HEIGHT, then check width
          finalWidth = Math.round(img.width * (MAX_HEIGHT / img.height));
        }
        if (finalWidth > MAX_WIDTH) {
          const minOverlapPx = Math.round(MAX_WIDTH * (OVERLAP_PERCENT / 100));
          const maxStep = MAX_WIDTH - minOverlapPx;
          numParts = Math.ceil((finalWidth - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
        }
      }

      // Generate split line elements (numParts - 1 lines at evenly distributed positions)
      let splitLinesHtml = "";
      if (showSplitLines && numParts > 1) {
        const lines = [];
        const splitClass = state === 1 ? "split-h" : "split-v";
        for (let i = 1; i < numParts; i++) {
          const pos = (i / numParts) * 100;
          lines.push(`<div class="split-line ${splitClass}" style="left:${pos}%"></div>`);
        }
        splitLinesHtml = `<div class="split-lines">${lines.join("")}</div>`;
      }

      // Build tooltip
      const stateText = stateLabels[state] || "Normal";
      const partsText = numParts > 1 ? ` (${numParts} parts)` : "";

      item.className = `image-item ${stateClasses[state]} ${rotateClass}`.trim();
      item.onclick = () => cycleImageState(img.path);
      item.title = `${img.width}×${img.height} - ${stateText}${partsText}`;
      item.innerHTML = `
          <span class="image-state-badge">${stateLabels[state] || "•"}</span>
          <div class="image-preview-overlay">
            ${splitLinesHtml}
          </div>
          <img src="${img.dataUrl}" alt="${img.name}" loading="lazy">
          <div class="image-name">${img.name}</div>
        `;
    }

    imageGrid.appendChild(item);
  });
}

/**
 * Cycle state for a single image
 * All non-locked images: 0 -> 1 -> 2 -> 3 -> 0
 */
function cycleImageState(imagePath) {
  const currentState = imageStates[imagePath] || 0;
  imageStates[imagePath] = (currentState + 1) % 4;
  renderImageGrid();
}

/**
 * Apply state to all eligible images based on smart rules
 * 0 = Normal (all selectable images)
 * 1 = H-Split (landscapes that canHSplit)
 * 2 = V-Split (all images that canVSplit - portraits AND landscapes)
 * 3 = Rotate (landscapes that don't fit screen)
 */
function applyStateToAll(state) {
  epubImagesCache.forEach((img) => {
    // Skip locked images
    if (img.isCover || img.isSeparator) return;

    const canHSplit = img.canHSplit && !img.fitsScreen;
    const canVSplit = img.canVSplit && !img.fitsScreen;
    const isLandscape = img.width > img.height;

    if (state === 0) {
      // Normal - applies to all selectable
      imageStates[img.path] = 0;
    } else if (state === 1) {
      // H-Split - all landscapes that can H-Split
      if (isLandscape && canHSplit) {
        imageStates[img.path] = 1;
      }
    } else if (state === 2) {
      // V-Split - all images that can V-Split (portrait and landscape)
      if (canVSplit) {
        imageStates[img.path] = 2;
      }
    } else if (state === 3) {
      // Rotate - landscapes that exceed screen
      if (isLandscape && !img.fitsScreen) {
        imageStates[img.path] = 3;
      }
    }
  });
  renderImageGrid();
}

/**
 * Start conversion with configured image states
 */
function startConversionWithImageStates() {
  const pickerSection = document.getElementById("imagePickerSection");
  const uploadBtn = document.getElementById("uploadBtn");
  const startConversionBtn = document.getElementById("startConversionBtn");

  // Hide picker and start conversion button, remove two-column layout
  pickerSection.style.display = "none";
  startConversionBtn.style.display = "none";
  document.querySelector("#uploadModal .modal").classList.remove("picker-mode");
  document.getElementById("pickerColumns").classList.remove("picker-active");

  // Show upload button and trigger upload
  uploadBtn.style.display = "block";
  uploadBtn.disabled = false;
  uploadFile();
}

/**
 * Get processing state for an image path
 * Returns 0 (Normal), 1 (H-Split), 2 (V-Split), or 3 (Rotate)
 */
function getImageState(imagePath) {
  return imageStates[imagePath] || 0;
}

/**
 * Get state label for logging
 */
function getStateLabel(state) {
  const labels = ["Normal", "H-Split", "V-Split", "Rotate"];
  return labels[state] || "Normal";
}

// Initialize quality settings handlers
document.addEventListener("DOMContentLoaded", function () {
  document.getElementById("file-table").addEventListener("click", function (event) {
    const link = event.target.closest(".image-preview-link");
    if (!link) return;
    event.preventDefault();
    openImagePreview(link.getAttribute("href"), link.textContent);
  });

  const qualitySlider = document.getElementById("qualitySlider");
  const qualityInput = document.getElementById("qualityInput");

  // Initialize advanced mode toggle state based on Optimize EPUB checkbox
  const convertCheckbox = document.getElementById("convertBeforeUpload");
  const advancedToggle = document.getElementById("advancedOptionsToggle");
  if (convertCheckbox && advancedToggle) {
    const isOptimizeEnabled = convertCheckbox.checked;
    advancedToggle.style.opacity = isOptimizeEnabled ? "1" : "0.5";
    advancedToggle.style.pointerEvents = isOptimizeEnabled ? "auto" : "none";
  }

  if (qualitySlider && qualityInput) {
    // Initialize converter variables without overwriting remembered settings.
    suppressUploadSettingsSave = true;
    try {
      updateQualitySettings();
    } finally {
      suppressUploadSettingsSave = false;
    }

    // Deselect all presets when slider is manually changed
    const deselectPresets = function () {
      document.querySelectorAll(".quality-preset").forEach((btn) => {
        btn.classList.remove("active");
      });
    };

    qualitySlider.oninput = function () {
      qualityInput.value = this.value;
      deselectPresets();
      updateQualitySettings();
    };
    qualityInput.onchange = function () {
      let v = Math.max(1, Math.min(95, parseInt(this.value, 10) || 85));
      this.value = v;
      qualitySlider.value = v;
      deselectPresets();
      updateQualitySettings();
    };
    qualityInput.onblur = function () {
      this.value = qualitySlider.value;
    };
  }
});

function openFolderModal() {
  document.getElementById("folderPathDisplay").textContent = currentPath === "/" ? "/ 🏠" : currentPath;
  document.getElementById("folderModal").classList.add("open");
  document.getElementById("folderName").value = "";
  setTimeout(() => document.getElementById("folderName").focus(), 50);
  document.getElementById("folderName").onkeydown = (e) => {
    if (e.key === "Enter" && e.target.value.trim()) createFolder();
  };
}

function closeFolderModal() {
  document.getElementById("folderModal").classList.remove("open");
}

// Toggle select-all checkbox
function toggleSelectAll(master) {
  const checked = master.checked;
  document.querySelectorAll(".select-item").forEach((cb) => {
    cb.checked = checked;
  });
}

function getSelectedItems() {
  const items = [];
  document.querySelectorAll(".select-item:checked").forEach((cb) => {
    items.push({
      name: cb.dataset.name || decodeURIComponent(cb.dataset.path).split("/").pop(),
      path: decodeURIComponent(cb.dataset.path),
      isFolder: cb.dataset.type === "folder",
    });
  });
  return items;
}

// Open delete modal for currently selected checkboxes
function openDeleteSelectedModal() {
  const items = getSelectedItems();
  if (items.length === 0) {
    alert("Please select at least one item to delete.");
    return;
  }
  openDeleteModalForItems(items);
}

// Open delete modal for a single item (keeps backwards compatibility with per-row delete button)
function openDeleteModal(name, path, isFolder) {
  openDeleteModalForItems([{ name: name, path: path, isFolder: !!isFolder }]);
}

let deleteItemsGlobal = [];

function openDeleteModalForItems(items) {
  deleteItemsGlobal = items;
  const listEl = document.getElementById("deleteItemList");
  listEl.innerHTML = "";
  items.forEach((it) => {
    const div = document.createElement("div");
    div.style.marginBottom = "6px";
    div.textContent = (it.isFolder ? "📁 " : "📄 ") + it.path;
    listEl.appendChild(div);
  });
  document.getElementById("deleteModal").classList.add("open");
}

function closeDeleteModal() {
  document.getElementById("deleteModal").classList.remove("open");
}

async function confirmDelete() {
  if (!deleteItemsGlobal || deleteItemsGlobal.length === 0) {
    closeDeleteModal();
    return;
  }

  const paths = deleteItemsGlobal.map((it) => {
    // Ensure path starts with /
    let p = it.path;
    if (!p.startsWith("/")) p = "/" + p;
    return p;
  });

  const deleteBtn = document.querySelector("#deleteModal .delete-btn-confirm");
  if (deleteBtn) {
    deleteBtn.disabled = true;
    deleteBtn.textContent = "Deleting...";
  }

  const failed = [];
  try {
    for (const path of paths) {
      const res = await fetch("/delete", {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: "path=" + encodeURIComponent(path),
      });
      if (!res.ok) {
        const text = await res.text();
        failed.push(path + ": " + text);
      }
    }

    if (failed.length === 0) {
      window.location.reload();
      return;
    }

    const shown = failed.slice(0, 3).join("\n");
    const hiddenCount = failed.length - 3;
    alert(
      "Failed to delete " +
        failed.length +
        " item" +
        (failed.length === 1 ? "" : "s") +
        ":\n" +
        shown +
        (hiddenCount > 0 ? "\n...and " + hiddenCount + " more" : ""),
    );
    closeDeleteModal();
  } catch (_) {
    alert("Failed to delete - network error");
    closeDeleteModal();
  } finally {
    if (deleteBtn) {
      deleteBtn.disabled = false;
      deleteBtn.textContent = "Delete";
    }
  }
}

// Helper to clear image picker state
function clearImagePicker() {
  epubImagesCache = [];
  imageStates = {};
  const imageGrid = document.getElementById("imageGrid");
  if (imageGrid) imageGrid.innerHTML = "";
  const pickerSection = document.getElementById("imagePickerSection");
  if (pickerSection) pickerSection.style.display = "none";
  // Reset two-column layout
  document.querySelector("#uploadModal .modal")?.classList.remove("picker-mode");
  document.getElementById("pickerColumns")?.classList.remove("picker-active");
  // Hide start conversion, show upload
  const startBtn = document.getElementById("startConversionBtn");
  if (startBtn) startBtn.style.display = "none";
  const uploadBtn = document.getElementById("uploadBtn");
  if (uploadBtn) uploadBtn.style.display = "block";
}

// Set up file input click listener once
(function setupFileInputListener() {
  const fileInput = document.getElementById("fileInput");
  if (!fileInput) return;

  fileInput.addEventListener("click", function () {
    // Small delay to allow browser to process click before checking files
    setTimeout(() => {
      if (fileInput.files.length === 0) {
        clearImagePicker();
        document.getElementById("uploadBtn").disabled = true;
      }
    }, 10);
  });

  // Drag-and-drop: route dropped files through the existing file input so the
  // normal validateFile() pipeline (EPUB optimize options, batch mode, upload)
  // runs unchanged.
  const dropZone = document.getElementById("dropZone");
  if (dropZone) {
    // dragenter/dragleave fire for child elements too; a counter avoids the
    // highlight flickering as the cursor moves over the hint text or input.
    let dragDepth = 0;

    const uploadBusy = () => typeof isUploadInProgress !== "undefined" && isUploadInProgress;

    dropZone.addEventListener("click", function (e) {
      if (uploadBusy()) return;
      if (e.target !== fileInput) fileInput.click();
    });

    dropZone.addEventListener("dragenter", function (e) {
      e.preventDefault();
      dragDepth++;
      if (!uploadBusy()) dropZone.classList.add("dragover");
    });

    dropZone.addEventListener("dragover", function (e) {
      // Required so the browser doesn't navigate to / open the dropped file.
      e.preventDefault();
    });

    dropZone.addEventListener("dragleave", function (e) {
      e.preventDefault();
      dragDepth = Math.max(0, dragDepth - 1);
      if (dragDepth === 0) dropZone.classList.remove("dragover");
    });

    dropZone.addEventListener("drop", function (e) {
      e.preventDefault();
      dragDepth = 0;
      dropZone.classList.remove("dragover");

      // Don't let a drop disrupt an in-flight transfer.
      if (uploadBusy()) return;

      const dropped = e.dataTransfer && e.dataTransfer.files;
      if (!dropped || dropped.length === 0) return;

      // Replace the current selection, matching native file-picker semantics.
      const dt = new DataTransfer();
      for (const file of dropped) dt.items.add(file);
      fileInput.files = dt.files;

      validateFile();
    });
  }
})();

function validateFile() {
  const fileInput = document.getElementById("fileInput");
  const uploadBtn = document.getElementById("uploadBtn");
  const files = fileInput.files;
  const convertOptions = document.getElementById("convertOptions");
  fileInput.classList.toggle("has-files", files.length > 0);

  // Show convert options only when at least one selected file is an EPUB.
  const hasEpub = Array.from(files).some((f) => f.name.toLowerCase().endsWith(".epub"));
  if (files.length > 0 && hasEpub) {
    convertOptions.style.display = "block";
    toggleConvertOptions();
  } else {
    convertOptions.style.display = "none";
    uploadBtn.textContent = "Upload";
    uploadBtn.classList.remove("optimize");
    document.getElementById("convertInfo").style.display = "none";
    document.getElementById("convertWarning").style.display = "none";
    if (files.length === 0) clearImagePicker();
  }

  if (files.length > 0) {
    // If advanced settings is expanded and single EPUB, show image picker
    const advancedContent = document.getElementById("advancedSettingsContent");
    const convertEnabled = document.getElementById("convertBeforeUpload").checked;
    if (
      advancedContent.classList.contains("visible") &&
      files.length === 1 &&
      convertEnabled &&
      files[0].name.toLowerCase().endsWith(".epub")
    ) {
      showImagePicker(files[0]).catch((err) => console.error("Image picker error:", err));
    } else {
      // New selection no longer matches single-EPUB-with-advanced-expanded —
      // tear down any picker from a previous file so the grid and picker-mode
      // layout don't linger. clearImagePicker is idempotent.
      clearImagePicker();
    }

    // If multiple files with conversion, inform user about batch mode
    if (files.length > 1 && convertEnabled) {
      const epubCount = Array.from(files).filter((f) => f.name.toLowerCase().endsWith(".epub")).length;
      if (epubCount > 0) {
        console.log(`Batch mode: ${epubCount} EPUB(s) will use auto settings`);
      }
    }

    updateBatchModeUI(files.length > 1);
    uploadBtn.disabled = isUploadInProgress;
  } else {
    updateBatchModeUI(false);
    uploadBtn.disabled = true;
  }
}

let failedUploadsGlobal = [];
let wsConnection = null;
let isUploadInProgress = false; // Prevent modal close during upload/conversion
let operationCancelled = false; // Set by Cancel to stop conversion loops and upload async handlers
let uploadGeneration = 0; // Incremented each uploadFile() call; guards stale restoreAfterCancel()
let currentUploadWs = null; // Active WebSocket reference for external abort
let currentUploadXhr = null; // Active XHR reference for external abort
// On-device HTTP uses port 80 and WebSocket uses 81. The simulator maps the
// same adjacent-port contract to an unprivileged host pair such as 8080/8081.
const HTTP_PORT = Number(window.location.port || 80);
const WS_PORT = HTTP_PORT + 1;
const WS_CHUNK_SIZE = 4096; // 4KB chunks - smaller for ESP32 stability

// ============================================================================
// EPUB Image Conversion Functions (from Baseline JPEG Converter)
// ============================================================================

// Device profiles (short edge × long edge in portrait orientation)
const DEVICE_PROFILES = {
  X4: { width: 480, height: 800, label: "X4" },
  X3: { width: 528, height: 792, label: "X3" },
};

// Default conversion settings
const DEFAULT_DEVICE = "X4";
const DEFAULT_MAX_WIDTH = DEVICE_PROFILES[DEFAULT_DEVICE].width;
const DEFAULT_MAX_HEIGHT = DEVICE_PROFILES[DEFAULT_DEVICE].height;
const DEFAULT_JPEG_QUALITY = 85;
const DEFAULT_ENABLE_GRAYSCALE = true;
const COVER_MAX_WIDTH = 480;
const COVER_MAX_HEIGHT = 792;
const X_DEFAULT_REFERENCE_CHARACTERS_PER_PAGE = 1500;
// Note: Overlap is now always centered distribution (min 5%)

// Dynamic conversion settings (updated by UI)
let DEVICE_TARGET = "auto"; // 'auto' | 'X4' | 'X3'
let DETECTED_DEVICE = null; // populated from /api/status
let ACTIVE_DEVICE = DEFAULT_DEVICE;
let MAX_WIDTH = DEFAULT_MAX_WIDTH;
let MAX_HEIGHT = DEFAULT_MAX_HEIGHT;
let JPEG_QUALITY = DEFAULT_JPEG_QUALITY;
let ENABLE_GRAYSCALE = DEFAULT_ENABLE_GRAYSCALE;
let HANDEDNESS = "right"; // 'right' = clockwise (right-handed), 'left' = counter-clockwise (left-handed)
let OVERLAP_PERCENT = 5; // Minimum overlap percentage for splits (5%, 10%, 15%)
const UPLOAD_SETTINGS_STORAGE_KEY = "crossink.files.uploadSettings.v1";
const DEFAULT_UPLOAD_SETTINGS = Object.freeze({
  convertBeforeUpload: false,
  renameFromMetadata: false,
  splitLongSections: true,
  preserveCoverColor: true,
  quality: DEFAULT_JPEG_QUALITY,
  referenceCharacters: X_DEFAULT_REFERENCE_CHARACTERS_PER_PAGE,
  deviceTarget: "auto",
  handedness: "right",
  overlap: 5,
  exportLog: false,
});
let suppressUploadSettingsSave = false;

function getCurrentUploadSettings() {
  return {
    convertBeforeUpload: !!document.getElementById("convertBeforeUpload")?.checked,
    renameFromMetadata: !!document.getElementById("renameFromMetadataToggle")?.checked,
    splitLongSections: !!document.getElementById("splitLongSectionsToggle")?.checked,
    preserveCoverColor: !!document.getElementById("preserveCoverColorToggle")?.checked,
    quality: parseInt(document.getElementById("qualitySlider")?.value || JPEG_QUALITY, 10),
    referenceCharacters: parseInt(
      document.getElementById("referenceCharactersInput")?.value || X_DEFAULT_REFERENCE_CHARACTERS_PER_PAGE,
      10,
    ),
    deviceTarget: DEVICE_TARGET,
    handedness: HANDEDNESS,
    overlap: OVERLAP_PERCENT,
    exportLog: !!document.getElementById("export-log-checkbox")?.checked,
  };
}

function applyUploadSettings(settings = {}) {
  const merged = { ...DEFAULT_UPLOAD_SETTINGS, ...settings };
  suppressUploadSettingsSave = true;
  try {
    document.getElementById("convertBeforeUpload").checked = !!merged.convertBeforeUpload;
    document.getElementById("renameFromMetadataToggle").checked = !!merged.renameFromMetadata;
    document.getElementById("splitLongSectionsToggle").checked = !!merged.splitLongSections;
    document.getElementById("preserveCoverColorToggle").checked = !!merged.preserveCoverColor;
    document.getElementById("export-log-checkbox").checked = !!merged.exportLog;
    document.getElementById("rememberUploadSettings").checked = !!settings.rememberSettings;
    document.getElementById("referenceCharactersInput").value = normalizedReferenceCharactersPerPage(
      merged.referenceCharacters,
    );

    setQualityPreset(Math.max(1, Math.min(95, parseInt(merged.quality, 10) || DEFAULT_JPEG_QUALITY)));
    setDeviceTarget(["auto", "X3", "X4"].includes(merged.deviceTarget) ? merged.deviceTarget : "auto");
    setHandedness(merged.handedness === "left" ? "left" : "right");
    setOverlap([5, 10, 15].includes(Number(merged.overlap)) ? Number(merged.overlap) : 5);
    toggleConvertOptions();
  } finally {
    suppressUploadSettingsSave = false;
  }
}

function restoreUploadSettingsFromStorage() {
  try {
    const saved = JSON.parse(localStorage.getItem(UPLOAD_SETTINGS_STORAGE_KEY) || "null");
    applyUploadSettings(saved?.rememberSettings ? saved : undefined);
  } catch (error) {
    console.warn("Could not read remembered upload settings:", error);
    applyUploadSettings();
  }
}

function updateUploadSettingsPersistence() {
  if (suppressUploadSettingsSave) return;
  try {
    if (!document.getElementById("rememberUploadSettings")?.checked) {
      localStorage.removeItem(UPLOAD_SETTINGS_STORAGE_KEY);
      return;
    }
    const settings = { ...getCurrentUploadSettings(), rememberSettings: true };
    localStorage.setItem(UPLOAD_SETTINGS_STORAGE_KEY, JSON.stringify(settings));
  } catch (error) {
    console.warn("Could not save upload settings:", error);
  }
}

// ============================================================================
// Image Picker State Management
// ============================================================================

let imageStates = {}; // Map: imagePath -> state (0=Normal, 1=H-Split, 2=V-Split, 3=Rotate)
let epubImagesCache = []; // Cache of extracted images for preview
let pendingConversionFile = null; // File awaiting conversion after image selection

// ============================================================================
// Enhanced Logging System
// ============================================================================

let logStartTime = null;
let conversionStats = {
  images: 0,
  splits: 0,
  splitParts: 0,
  fixes: 0,
  skipped: 0,
  errors: 0,
  originalSize: 0,
  newSize: 0,
};
const logSection = document.getElementById("log-section");
const logContainer = document.getElementById("log-container");
const exportLogCheckbox = document.getElementById("export-log-checkbox");

// CrossInk version (fetched from API)
let crosspointVersion = "Unknown";

// Fetch version from API
async function fetchVersion() {
  try {
    const response = await fetch("/api/status");
    if (response.ok) {
      const data = await response.json();
      crosspointVersion = data.version || "Unknown";
      if (data.device === "X3" || data.device === "X4") {
        DETECTED_DEVICE = data.device;
        applyDeviceTarget();
      }
    }
  } catch (e) {
    console.error("Failed to fetch version:", e);
  }
}

// Resolve DEVICE_TARGET ('auto' | 'X4' | 'X3') to a concrete profile and update UI.
function applyDeviceTarget() {
  const resolved = DEVICE_TARGET === "auto" ? DETECTED_DEVICE || DEFAULT_DEVICE : DEVICE_TARGET;
  const profile = DEVICE_PROFILES[resolved] || DEVICE_PROFILES[DEFAULT_DEVICE];
  ACTIVE_DEVICE = resolved;
  MAX_WIDTH = profile.width;
  MAX_HEIGHT = profile.height;

  const summary = document.getElementById("convertSizeSummary");
  if (summary) {
    summary.textContent = `📏 Max ${profile.width}×${profile.height}px`;
  }
  document.querySelectorAll(".device-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.value === DEVICE_TARGET);
  });
  const autoLabel = document.getElementById("deviceAutoLabel");
  if (autoLabel) {
    autoLabel.textContent = DETECTED_DEVICE ? `Auto (${DETECTED_DEVICE})` : "Auto";
  }

  // Recompute picker classification with new dimensions, then refresh grid.
  if (Array.isArray(epubImagesCache) && epubImagesCache.length > 0) {
    for (const img of epubImagesCache) {
      img.fitsScreen = img.width <= MAX_WIDTH && img.height <= MAX_HEIGHT;
      img.canHSplit = img.width >= MAX_HEIGHT;
      img.canVSplit = img.height >= MAX_HEIGHT;
    }
    const pickerSection = document.getElementById("imagePickerSection");
    if (pickerSection && pickerSection.style.display !== "none" && typeof renderImageGrid === "function") {
      renderImageGrid();
    }
  }
}

function setDeviceTarget(value) {
  DEVICE_TARGET = value;
  applyDeviceTarget();
  updateUploadSettingsPersistence();
}

// Batch logging system for multiple files
let batchLogEntries = [];
let batchStats = {
  filesProcessed: 0,
  filesSucceeded: 0,
  filesFailed: 0,
  totalImages: 0,
  totalSplits: 0,
  totalFixes: 0,
  totalErrors: 0,
  totalOriginalSize: 0,
  totalNewSize: 0,
};
let batchStartTime = null;
let isBatchMode = false;

// Format bytes to human-readable size (for logging)
function formatBytes(b) {
  if (!b) return "0 B";
  const k = 1024;
  const s = ["B", "KB", "MB", "GB"];
  const i = Math.floor(Math.log(b) / Math.log(k));
  return (b / Math.pow(k, i)).toFixed(1) + " " + s[i];
}

// Get elapsed timestamp since log start
function getTimestamp() {
  if (!logStartTime) return "[00:00.0]";
  const elapsed = (Date.now() - logStartTime) / 1000;
  const mins = Math.floor(elapsed / 60)
    .toString()
    .padStart(2, "0");
  const secs = (elapsed % 60).toFixed(1).padStart(4, "0");
  return `[${mins}:${secs}]`;
}

// Main logging function
function log(message, type = "", tag = "") {
  const entry = document.createElement("div");
  entry.className = "log-entry " + type;

  // Timestamp
  const timestamp = document.createElement("span");
  timestamp.className = "log-timestamp";
  timestamp.textContent = getTimestamp();
  entry.appendChild(timestamp);

  // Tag (if provided)
  if (tag) {
    const tagEl = document.createElement("span");
    tagEl.className = "log-tag " + tag.toLowerCase();
    tagEl.textContent = tag;
    entry.appendChild(tagEl);
  }

  // Message
  const msg = document.createElement("span");
  msg.className = "log-message";
  msg.innerHTML = message;
  entry.appendChild(msg);

  logContainer.appendChild(entry);
  logContainer.scrollTop = logContainer.scrollHeight;
}

// Log image processing details
function logImage(
  name,
  origW,
  origH,
  origFormat,
  origSize,
  newW,
  newH,
  newSize,
  wasSplit = false,
  splitCount = 0,
  partsInfo = null,
  imageState = 0,
) {
  const saved = origSize - newSize;
  const savedPct = ((saved / origSize) * 100).toFixed(0);
  const dims = `${origW}×${origH}`;
  const newDims = `${newW}×${newH}`;

  // Get state label and color
  const stateLabels = ["", "H-Split", "V-Split", "Rotate"];
  const stateColors = ["", "#3498db", "#e74c3c", "#9b59b6"];
  const stateLabel = stateLabels[imageState] || "";
  const stateColor = stateColors[imageState] || "";

  if (wasSplit) {
    conversionStats.splits++;
    conversionStats.splitParts += splitCount;
    // Build parts detail string
    let partsDetail = "";
    if (partsInfo && partsInfo.length > 0) {
      const baseName = name.replace(/\.[^.]+$/, "");
      partsDetail = partsInfo
        .map((p) => `${baseName}${p.suffix}.jpg (${p.width}×${p.height}, ${formatBytes(p.size)})`)
        .join(", ");
    }
    const savedInfo = saved > 0 ? `, <span style="color:#27ae60">-${savedPct}%</span>` : "";
    const stateIndicator =
      imageState > 0 ? ` <span style="color:${stateColor};font-weight:600;">[${stateLabel}]</span>` : "";
    log(
      `<strong>${escapeHtml(name)}</strong>${stateIndicator} <span class="log-detail">(${dims} ${origFormat.toUpperCase()}, ${formatBytes(origSize)}) → ${splitCount} parts${savedInfo}</span>`,
      "",
      "SPLIT",
    );
    if (partsDetail) {
      log(`<span class="log-detail" style="margin-left: 20px;">↳ ${partsDetail}</span>`, "", "");
    }
  } else {
    conversionStats.images++;
    const stateIndicator =
      imageState > 0 ? ` <span style="color:${stateColor};font-weight:600;">[${stateLabel}]</span>` : "";
    const detail =
      saved > 0
        ? `<span class="log-detail">(${dims} → ${newDims}, ${formatBytes(origSize)} → ${formatBytes(newSize)}, <span style="color:#27ae60">-${savedPct}%</span>)</span>`
        : `<span class="log-detail">(${dims} → ${newDims}, ${formatBytes(newSize)})</span>`;
    log(`<strong>${escapeHtml(name)}</strong>${stateIndicator} ${detail}`, "", "CONVERT");
  }
}

// Log fix applied
function logFix(type, detail) {
  conversionStats.fixes++;
  log(`${type}: <span class="log-detail">${detail}</span>`, "success", "FIX");
}

// Log skipped item
function logSkip(name, reason) {
  conversionStats.skipped++;
  log(`${escapeHtml(name)} <span class="log-detail">(${reason})</span>`, "", "SKIP");
}

// Log error
function logError(message) {
  conversionStats.errors++;
  log(message, "error", "ERROR");
}

// Log summary table
function logSummary(originalSize, newSize, timeElapsed) {
  const saved = originalSize - newSize;
  const savedPct = ((saved / originalSize) * 100).toFixed(1);
  const totalImages = conversionStats.images + conversionStats.splits;
  const totalOutput = conversionStats.images + conversionStats.splitParts;

  const summaryHtml = `
    <div class="log-summary">
      <div class="log-summary-title">📊 Conversion Summary</div>
      <table class="log-summary-table">
        <tr><td>Images found</td><td class="highlight">${totalImages}</td></tr>
        <tr><td>Images processed</td><td>${totalOutput}${conversionStats.splitParts > conversionStats.splits ? ` (+${conversionStats.splitParts - conversionStats.splits} from splits)` : ""}</td></tr>
        <tr><td>EPUB repairs</td><td>${conversionStats.fixes > 0 ? conversionStats.fixes + " fixes applied" : "None needed"}</td></tr>
        ${conversionStats.errors > 0 ? `<tr><td>Errors</td><td style="color:#e74c3c">${conversionStats.errors}</td></tr>` : ""}
        <tr><td>Original size</td><td>${formatBytes(originalSize)}</td></tr>
        <tr><td>Optimized size</td><td>${formatBytes(newSize)}</td></tr>
        <tr><td>Saved</td><td class="${saved > 0 ? "saved" : "increased"}">${saved > 0 ? formatBytes(saved) + " (" + savedPct + "%)" : "+" + formatBytes(-saved)}</td></tr>
        <tr><td>Time</td><td>${timeElapsed.toFixed(1)}s</td></tr>
      </table>
    </div>
  `;
  logContainer.insertAdjacentHTML("beforeend", summaryHtml);
  logContainer.scrollTop = logContainer.scrollHeight;
}

// Clear log
function clearLog() {
  logContainer.innerHTML = "";
  logStartTime = Date.now();
  conversionStats = {
    images: 0,
    splits: 0,
    splitParts: 0,
    fixes: 0,
    skipped: 0,
    errors: 0,
    originalSize: 0,
    newSize: 0,
  };
}

// Start batch logging mode
function startBatchLog(fileCount) {
  isBatchMode = true;
  batchStartTime = Date.now();
  batchLogEntries = [];
  batchStats = {
    filesProcessed: 0,
    filesSucceeded: 0,
    filesFailed: 0,
    totalImages: 0,
    totalSplits: 0,
    totalFixes: 0,
    totalErrors: 0,
    totalOriginalSize: 0,
    totalNewSize: 0,
  };
  clearLog();
  logContainer.innerHTML = ""; // Clear display
  log(`Starting batch conversion: ${fileCount} file(s)`, "", "INFO");
}

// Save current file's log to batch entries
function saveToFileBatchLog(fileName, succeeded, originalSize = 0, newSize = 0) {
  if (!isBatchMode) return;

  const entries = Array.from(logContainer.querySelectorAll(".log-entry"));
  batchLogEntries.push({
    fileName: fileName,
    succeeded: succeeded,
    entries: entries,
    stats: { ...conversionStats },
  });

  // Update batch stats
  batchStats.filesProcessed++;
  if (succeeded) {
    batchStats.filesSucceeded++;
  } else {
    batchStats.filesFailed++;
  }
  batchStats.totalImages += conversionStats.images;
  batchStats.totalSplits += conversionStats.splits;
  batchStats.totalFixes += conversionStats.fixes;
  batchStats.totalErrors += conversionStats.errors;
  // Defaults of 0 keep failure-path callers safe — files that never
  // produced a converted blob contribute nothing to the totals.
  batchStats.totalOriginalSize += originalSize;
  batchStats.totalNewSize += newSize;

  // Clear for next file
  logContainer.innerHTML = "";
  conversionStats = {
    images: 0,
    splits: 0,
    splitParts: 0,
    fixes: 0,
    skipped: 0,
    errors: 0,
    originalSize: 0,
    newSize: 0,
  };
}

// Finalize batch log and export
function finalizeBatchLog() {
  if (!isBatchMode) return;

  const batchTime = (Date.now() - batchStartTime) / 1000;

  // Build consolidated log display
  logContainer.innerHTML = "";
  log(`Starting batch conversion: ${batchStats.filesProcessed} file(s)`, "", "INFO");

  // Add all file entries
  batchLogEntries.forEach((fileLog, index) => {
    const fileHeader = document.createElement("div");
    fileHeader.className = "log-entry";
    fileHeader.style.marginTop = index > 0 ? "15px" : "5px";
    fileHeader.style.borderTop = index > 0 ? "1px solid var(--border-color)" : "none";
    fileHeader.style.paddingTop = index > 0 ? "10px" : "0";
    fileHeader.innerHTML = `<span class="log-timestamp"></span><span class="log-message"><strong>${escapeHtml(fileLog.fileName)}</strong> — ${fileLog.succeeded ? '<span style="color:#27ae60">✓ Success</span>' : '<span style="color:#e74c3c">✗ Failed</span>'}</span>`;
    logContainer.appendChild(fileHeader);

    fileLog.entries.forEach((entry) => {
      const clone = entry.cloneNode(true);
      logContainer.appendChild(clone);
    });
  });

  // Aggregate size totals: only emit rows when at least one file was successfully
  // converted (totalOriginalSize stays 0 for batches where conversion was off or
  // every file fell back to original upload).
  const totalSaved = batchStats.totalOriginalSize - batchStats.totalNewSize;
  const totalSavedPct =
    batchStats.totalOriginalSize > 0 ? ((totalSaved / batchStats.totalOriginalSize) * 100).toFixed(1) : "0.0";
  const sizeRowsHtml =
    batchStats.totalOriginalSize > 0
      ? `
        <tr><td>Total original</td><td>${formatBytes(batchStats.totalOriginalSize)}</td></tr>
        <tr><td>Total optimised</td><td>${formatBytes(batchStats.totalNewSize)}</td></tr>
        <tr><td>Total saved</td><td class="${totalSaved > 0 ? "saved" : "increased"}">${
          totalSaved > 0 ? `${formatBytes(totalSaved)} (${totalSavedPct}%)` : `+${formatBytes(-totalSaved)}`
        }</td></tr>`
      : "";

  // Add batch summary
  const batchSummaryHtml = `
    <div class="log-summary">
      <div class="log-summary-title">📊 Batch Conversion Summary</div>
      <table class="log-summary-table">
        <tr><td>Files processed</td><td class="highlight">${batchStats.filesProcessed}</td></tr>
        <tr><td>Successful</td><td style="color:#27ae60">${batchStats.filesSucceeded}</td></tr>
        <tr><td>Failed</td><td style="${batchStats.filesFailed > 0 ? "#e74c3c" : "#7f8c8d"}">${batchStats.filesFailed}</td></tr>
        <tr><td>Total images processed</td><td>${batchStats.totalImages}</td></tr>
        <tr><td>Total splits</td><td>${batchStats.totalSplits}</td></tr>
        <tr><td>Total fixes applied</td><td>${batchStats.totalFixes}</td></tr>
        ${batchStats.totalErrors > 0 ? `<tr><td>Total errors</td><td style="color:#e74c3c">${batchStats.totalErrors}</td></tr>` : ""}${sizeRowsHtml}
        <tr><td>Total time</td><td>${batchTime.toFixed(1)}s</td></tr>
      </table>
    </div>
  `;
  logContainer.insertAdjacentHTML("beforeend", batchSummaryHtml);
  logContainer.scrollTop = logContainer.scrollHeight;

  // Auto-export if checkbox is checked
  if (exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, true); // isBatch = true
    }, 200);
  }

  // Reset batch mode
  isBatchMode = false;
  batchLogEntries = [];
}

// Show/hide log section
function showLog() {
  logSection.classList.add("visible");
}

function hideLog() {
  logSection.classList.remove("visible");
}

// Generate standardized log filename with date
function generateLogFilename(isBatch = false) {
  const now = new Date();
  const date = now.toISOString().split("T")[0]; // YYYY-MM-DD
  const time = now.toTimeString().split(" ")[0].replace(/:/g, "-"); // HH-MM-SS
  const prefix = isBatch ? "batch" : "epub";
  return `${prefix}-conversion-log-${date}_${time}.txt`;
}

// Export log as text file (can be called automatically)
function exportLogToFile(filename = null, isBatch = false) {
  // Use standardized filename if none provided
  if (!filename) {
    filename = generateLogFilename(isBatch);
  }
  // Extract text from log entries
  const entries = logContainer.querySelectorAll(".log-entry");
  let logText = `CrossInk Reader ${crosspointVersion} - EPUB Conversion Log\n`;
  logText += `Generated: ${new Date().toLocaleString()}\n`;
  logText += `${"=".repeat(60)}\n\n`;

  entries.forEach((entry) => {
    const timestamp = entry.querySelector(".log-timestamp")?.textContent || "";
    const tag = entry.querySelector(".log-tag")?.textContent || "";
    const message = entry.querySelector(".log-message")?.textContent || entry.textContent;

    if (tag) {
      logText += `${timestamp} [${tag}] ${message}\n`;
    } else {
      logText += `${timestamp} ${message}\n`;
    }
  });

  // Extract summary table if present
  const summary = logContainer.querySelector(".log-summary");
  if (summary) {
    logText += `\n${"=".repeat(60)}\n`;
    const title = summary.querySelector(".log-summary-title")?.textContent || "Summary";
    logText += `${title}\n`;
    logText += `${"-".repeat(40)}\n`;

    const rows = summary.querySelectorAll("tr");
    rows.forEach((row) => {
      const cells = row.querySelectorAll("td");
      if (cells.length >= 2) {
        logText += `${cells[0].textContent.padEnd(25)} ${cells[1].textContent}\n`;
      }
    });
  }

  // Create download link
  const blob = new Blob([logText], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename || `epub-conversion-log-${Date.now()}.txt`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

// ═══════════════════════════════════════════════════════════════════
// EPUB Utilities — ported from EPUB Optimizer Pro
// ═══════════════════════════════════════════════════════════════════

/** Defensive CSS injected into every XHTML <head> — prevents e-ink overflow. */
const DEFENSIVE_STYLE =
  '<style type="text/css">img,svg{max-width:100%;height:auto}body{overflow-wrap:break-word}table{max-width:100%;table-layout:fixed}pre,code{white-space:pre-wrap;word-wrap:break-word}*{box-sizing:border-box}</style>';
const X_LOCATION_MANIFEST_PATH = "META-INF/x-locations.json";
const X_LOCATION_WORDS_PER_UNIT = 64;
const SECTION_SPLIT_WORD_THRESHOLD = 8000;
const SECTION_SPLIT_BYTE_THRESHOLD = 32768;
const SECTION_SPLIT_HARD_BYTE_LIMIT = 49152;
// Let an oversized section run a little past its preferred size when that reaches an
// author-supplied break. The hard limit still protects the reader's section builder.
const SECTION_SPLIT_NATURAL_BOUNDARY_LOOKAHEAD_BYTES = 8192;
const SECTION_SPLIT_SUFFIX_RE = /__ci_section_\d{3}(?=\.[^.]+$)/i;
const XHTML_NS = "http://www.w3.org/1999/xhtml";
const OPF_NS = "http://www.idpf.org/2007/opf";
const DEFLATE_OPTS = { compression: "DEFLATE", compressionOptions: { level: 8 }, createFolders: false };

/** First defined namespaceURI walking node -> ancestors, else the fallback. */
function inheritedNs(nodes, fallback) {
  for (const node of nodes) if (node && node.namespaceURI) return node.namespaceURI;
  return fallback;
}

/** Escape a string for safe insertion into XML attribute values / text content. */
function xmlEscape(str) {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function escapeRegex(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

/**
 * Decode a URI-encoded href (e.g., "my%20image.jpg" → "my image.jpg").
 * Handles double-encoding gracefully.
 */
function decodeHref(href) {
  try {
    return decodeURIComponent(href);
  } catch (e) {
    return href;
  }
}

const SCRUBBED_BLANK_CODEPOINT_RANGES = [
  [0x0000, 0x0008],
  [0x000b, 0x000c],
  [0x000e, 0x001f],
  [0x007f, 0x009f],
  [0x00ad, 0x00ad],
  [0x034f, 0x034f],
  [0x061c, 0x061c],
  [0x180b, 0x180f],
  [0x200b, 0x200f],
  [0x202a, 0x202e],
  [0x2060, 0x2064],
  [0x2066, 0x206f],
  [0xfe00, 0xfe0f],
  [0xfeff, 0xfeff],
];

const SCRUBBED_BLANK_CODEPOINT_RE =
  /[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F-\u009F\u00AD\u034F\u061C\u180B-\u180F\u200B-\u200F\u202A-\u202E\u2060-\u2064\u2066-\u206F\uFE00-\uFE0F\uFEFF]/g;
const NUMERIC_CHARACTER_REFERENCE_RE = /&#(?:x([0-9A-Fa-f]+)|([0-9]+));/g;
function isScrubbedBlankCodepoint(codePoint) {
  return SCRUBBED_BLANK_CODEPOINT_RANGES.some(([start, end]) => codePoint >= start && codePoint <= end);
}

function scrubBlankCodepoints(text) {
  let count = 0;
  const withoutLiterals = text.replace(SCRUBBED_BLANK_CODEPOINT_RE, () => {
    count++;
    return "";
  });

  const cleaned = withoutLiterals.replace(NUMERIC_CHARACTER_REFERENCE_RE, (match, hexValue, decimalValue) => {
    const rawValue = hexValue || decimalValue;
    const codePoint = Number.parseInt(rawValue, hexValue ? 16 : 10);
    if (Number.isFinite(codePoint) && isScrubbedBlankCodepoint(codePoint)) {
      count++;
      return "";
    }
    return match;
  });

  return { text: cleaned, count };
}

function scrubEpubTextResource(path, text) {
  const scrubbed = scrubBlankCodepoints(text);
  if (scrubbed.count > 0) {
    logFix("Blank codepoints", `${escapeHtml(path.split("/").pop())} (${scrubbed.count} removed)`);
  }
  return scrubbed.text;
}

/**
 * Safely read a text file from the zip, handling BOM and encoding.
 * Strips UTF-8 BOM. Detects encoding from XML declaration or meta tag.
 */
async function safeReadText(fileObj) {
  const raw = await fileObj.async("uint8array");

  // Detect and strip UTF-8 BOM (EF BB BF)
  let offset = 0;
  if (raw.length >= 3 && raw[0] === 0xef && raw[1] === 0xbb && raw[2] === 0xbf) {
    offset = 3;
  }

  // Try UTF-8 first (vast majority of EPUBs)
  const utf8 = new TextDecoder("utf-8", { fatal: true });
  try {
    return utf8.decode(raw.subarray(offset));
  } catch (e) {
    /* not valid UTF-8 */
  }

  // Peek at XML declaration or meta charset for encoding hint
  const ascii = new TextDecoder("ascii", { fatal: false }).decode(raw.subarray(offset, offset + 512));
  const encodingMatch = ascii.match(/encoding=["']([^"']+)["']/i) || ascii.match(/charset=["']?([^"'\s;]+)/i);
  const encoding = encodingMatch ? encodingMatch[1].toLowerCase() : "windows-1252";

  try {
    return new TextDecoder(encoding, { fatal: false }).decode(raw.subarray(offset));
  } catch (e) {
    // Last resort: lossy latin1
    return new TextDecoder("iso-8859-1", { fatal: false }).decode(raw.subarray(offset));
  }
}

/**
 * Find the canonical OPF path by parsing META-INF/container.xml.
 * Falls back to scanning for any .opf file.
 */
async function findOPFPath(zip) {
  try {
    const containerPath = Object.keys(zip.files).find((p) => p.toLowerCase() === "meta-inf/container.xml");
    if (containerPath) {
      const containerXml = await zip.files[containerPath].async("string");
      const match = containerXml.match(/<rootfile[^>]+full-path=["']([^"']+)["']/i);
      if (match && zip.files[match[1]]) return match[1];
    }
  } catch (e) {
    /* fall through */
  }
  let fallback = null;
  zip.forEach((p) => {
    if (!fallback && p.toLowerCase().endsWith(".opf")) fallback = p;
  });
  return fallback;
}

function sanitizeMetadataFilenamePart(value) {
  const text = String(value || "")
    .replace(/\s+/g, " ")
    .trim();
  return (text.normalize ? text.normalize("NFC") : text)
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, " ")
    .replace(/\s+/g, " ")
    .replace(/^[. ]+/g, "")
    .replace(/[. ]+$/g, "")
    .trim();
}

function buildMetadataFilename(title, author) {
  title = sanitizeMetadataFilenamePart(title);
  author = sanitizeMetadataFilenamePart(author);
  if (!title) return "";
  let base = author ? `${title} - ${author}` : title;
  if (base.length > 180) {
    base =
      base
        .substring(0, 180)
        .replace(/\s+\S*$/g, "")
        .trim() || base.substring(0, 180).trim();
  }
  return `${base}.epub`;
}

async function getMetadataFilenameForEpub(file) {
  if (typeof JSZip === "undefined") return "";
  const zip = await JSZip.loadAsync(file);
  const opfPath = await findOPFPath(zip);
  if (!opfPath || !zip.files[opfPath]) return "";

  const doc = new DOMParser().parseFromString(await safeReadText(zip.files[opfPath]), "application/xml");
  if (doc.querySelector("parsererror")) return "";

  const title = doc.getElementsByTagNameNS("*", "title")[0]?.textContent || "";
  const creators = Array.from(doc.getElementsByTagNameNS("*", "creator"));
  const getCreatorRole = (element) =>
    (
      element.getAttribute("role") ||
      element.getAttribute("opf:role") ||
      element.getAttributeNS("http://www.idpf.org/2007/opf", "role") ||
      ""
    ).toLowerCase();
  const authorElement =
    creators.find((element) => getCreatorRole(element) === "aut") ||
    creators.find((element) => !getCreatorRole(element));
  const authorText = authorElement?.textContent?.trim();
  const author =
    authorText ||
    authorElement?.getAttribute("file-as") ||
    authorElement?.getAttribute("opf:file-as") ||
    authorElement?.getAttributeNS("http://www.idpf.org/2007/opf", "file-as") ||
    "";
  return buildMetadataFilename(title, author);
}

async function maybeRenameEbookFile(file) {
  const renameToggle = document.getElementById("renameFromMetadataToggle");
  if (!renameToggle || !renameToggle.checked || !file.name.toLowerCase().endsWith(".epub")) return file;

  try {
    const metadataName = await getMetadataFilenameForEpub(file);
    if (!metadataName || metadataName === file.name) return file;
    return new File([file], metadataName, {
      type: file.type || "application/epub+zip",
      lastModified: file.lastModified,
    });
  } catch (error) {
    console.warn("Could not rename EPUB from metadata:", error);
    return file;
  }
}

function reserveAvailableUploadFilename(fileName, usedFileNames) {
  const normalize = (name) => name.toLowerCase();
  if (!usedFileNames.has(normalize(fileName))) {
    usedFileNames.add(normalize(fileName));
    return fileName;
  }

  const dotIndex = fileName.lastIndexOf(".");
  const extensionIndex = dotIndex > 0 ? dotIndex : fileName.length;
  const baseName = fileName.substring(0, extensionIndex);
  const extension = fileName.substring(extensionIndex);
  let nextSuffix = 2;
  let candidateName;
  do {
    candidateName = `${baseName} (${nextSuffix})${extension}`;
    nextSuffix++;
  } while (usedFileNames.has(normalize(candidateName)));

  usedFileNames.add(normalize(candidateName));
  return candidateName;
}

async function fetchExistingUploadNames() {
  const response = await fetch(`/api/files?path=${encodeURIComponent(currentPath)}&_=${Date.now()}`);
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  const entries = await response.json();
  return new Set(entries.map((entry) => entry.name.toLowerCase()));
}

/**
 * Resolve a relative href against a base file path.
 * Handles multiple ../, ./, absolute /, and bare relative paths.
 */
function resolvePath(basePath, href) {
  if (href.startsWith("/")) return href.substring(1);
  href = href.replace(/^\.\//, "");
  const baseDir = basePath.includes("/") ? basePath.substring(0, basePath.lastIndexOf("/")) : "";
  const baseParts = baseDir ? baseDir.split("/") : [];
  const hrefParts = href.split("/");
  while (hrefParts.length > 0 && hrefParts[0] === "..") {
    hrefParts.shift();
    if (baseParts.length > 0) baseParts.pop();
  }
  const resolved = [...baseParts, ...hrefParts].join("/");
  return resolved.replace(/\/+/g, "/");
}

function renamedImageSrc(src, xhtmlPath, renamed, splitImages = {}) {
  const renameEntries = Object.entries(renamed);
  if (renameEntries.length === 0) return { src, changed: false };

  const decodedSrc = decodeHref(src);
  const resolvedSrc = resolvePath(xhtmlPath, decodedSrc);
  const matchEntry = renameEntries.find(([oldPath, newPath]) => resolvedSrc === oldPath || resolvedSrc === newPath);
  if (!matchEntry) return { src, changed: false };

  const [oldPath, newPath] = matchEntry;
  const oldName = oldPath.split("/").pop();
  const mappedName = newPath.split("/").pop();
  const splitParts = splitImages[oldPath]?.parts || [];
  const targetName = splitParts[0]?.imgName || mappedName;

  let updatedSrc = decodedSrc.replace(oldName, targetName);
  if (updatedSrc === decodedSrc) updatedSrc = decodedSrc.replace(mappedName, targetName);
  if (updatedSrc === decodedSrc) return { src, changed: false };

  return { src: updatedSrc, changed: true };
}

function rewriteImageSrcReferences(content, xhtmlPath, renamed, splitImages = {}) {
  let changed = false;
  const rewritten = content.replace(
    /(<(?:\w+:)?img\b[^>]*?\bsrc\s*=\s*)(["'])([^"']+)\2/gi,
    (match, prefix, quote, src) => {
      const renamedSrc = renamedImageSrc(src, xhtmlPath, renamed, splitImages);
      if (!renamedSrc.changed) return match;

      changed = true;
      return `${prefix}${quote}${renamedSrc.src}${quote}`;
    },
  );

  return { content: rewritten, changed };
}

/** Remove XML comments from XHTML source text. Returns { text, count, bytes }. */
function stripComments(xml) {
  const skippedSections = [
    { open: "<![CDATA[", close: "]]>" },
    { open: "<?", close: "?>" },
  ];
  const encoder = new TextEncoder();
  const out = [];
  let index = 0;
  let count = 0;
  let bytes = 0;

  const doctypeEnd = (from) => {
    const gt = xml.indexOf(">", from);
    const subset = xml.indexOf("[", from);
    if (subset < 0 || (gt >= 0 && subset > gt)) return gt < 0 ? xml.length : gt + 1;
    const close = xml.indexOf("]>", subset);
    return close < 0 ? xml.length : close + 2;
  };

  while (index < xml.length) {
    const next = xml.indexOf("<", index);
    if (next < 0) {
      out.push(xml.slice(index));
      break;
    }
    if (next > index) out.push(xml.slice(index, next));

    if (xml.startsWith("<!--", next)) {
      const close = xml.indexOf("-->", next + 4);
      const end = close < 0 ? xml.length : close + 3;
      count++;
      bytes += encoder.encode(xml.slice(next, end)).length;
      index = end;
      continue;
    }

    if (xml.startsWith("<!DOCTYPE", next)) {
      const end = doctypeEnd(next + 9);
      out.push(xml.slice(next, end));
      index = end;
      continue;
    }

    const skipped = skippedSections.find((section) => xml.startsWith(section.open, next));
    if (skipped) {
      const close = xml.indexOf(skipped.close, next + skipped.open.length);
      const end = close < 0 ? xml.length : close + skipped.close.length;
      out.push(xml.slice(next, end));
      index = end;
      continue;
    }

    out.push("<");
    index = next + 1;
  }

  return { text: out.join(""), count, bytes };
}

/**
 * Serialize an XML doc back to string, preserving the original <?xml?> declaration
 * and cleaning up XMLSerializer namespace prefix noise (xmlns:ns0 etc).
 */
function safeSerialize(doc, originalContent) {
  let result = new XMLSerializer().serializeToString(doc);

  // Restore <?xml?> declaration if original had one
  if (originalContent && /^\s*<\?xml\b/.test(originalContent) && !/^\s*<\?xml\b/.test(result)) {
    const declMatch = originalContent.match(/^\s*(<\?xml[^?]*\?>)/);
    if (declMatch) result = declMatch[1] + "\n" + result;
  }

  // Clean up XMLSerializer namespace prefix noise (xmlns:ns0="..." ns0:attr="...")
  result = result.replace(/ xmlns:ns\d+="[^"]*"/g, "");
  result = result.replace(/ ns\d+:/g, " ");

  return result;
}

function findXmlDoctypeEnd(content, start) {
  let quote = null;
  let bracketDepth = 0;
  for (let i = start; i < content.length; i++) {
    const ch = content[i];
    if (quote) {
      if (ch === quote) quote = null;
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
    } else if (ch === "[") {
      bracketDepth++;
    } else if (ch === "]" && bracketDepth > 0) {
      bracketDepth--;
    } else if (ch === ">" && bracketDepth === 0) {
      return i + 1;
    }
  }
  return -1;
}

function findXmlRootElementStart(content) {
  let index = 0;
  while (index < content.length) {
    while (index < content.length && /\s/.test(content[index])) index++;

    if (content.startsWith("<!--", index)) {
      const end = content.indexOf("-->", index + 4);
      if (end < 0) return index;
      index = end + 3;
      continue;
    }

    if (content.startsWith("<?", index)) {
      const end = content.indexOf("?>", index + 2);
      if (end < 0) return index;
      index = end + 2;
      continue;
    }

    if (content.substring(index, index + 9).toLowerCase() === "<!doctype") {
      const end = findXmlDoctypeEnd(content, index + 9);
      if (end < 0) return index;
      index = end;
      continue;
    }

    if (content[index] === "<") return index;
    return 0;
  }
  return 0;
}

function protectWhitespaceOnlyTextNodes(content) {
  const preserved = [];
  const tokenPrefix = "__CROSSINK_PRESERVE_WS_";
  const rootStart = findXmlRootElementStart(content);
  const protectedContent = content.replace(/>([\s\u00a0]+)</g, (match, whitespace, offset) => {
    if (offset < rootStart) return match;

    const token = `${tokenPrefix}${preserved.length}__`;
    preserved.push(whitespace);
    return `>${token}<`;
  });

  return {
    content: protectedContent,
    restore(serialized) {
      return serialized.replace(new RegExp(`${escapeRegex(tokenPrefix)}(\\d+)__`, "g"), (match, indexText) => {
        const index = Number(indexText);
        return Number.isInteger(index) && index >= 0 && index < preserved.length ? preserved[index] : match;
      });
    },
  };
}

function localName(node) {
  return (node && (node.localName || node.nodeName || "")).toLowerCase();
}

function findFirstByLocalName(root, name) {
  const target = name.toLowerCase();
  const nodes = root.getElementsByTagName("*");
  for (const node of nodes) {
    if (localName(node) === target) return node;
  }
  return null;
}

function normalizeZipPath(path) {
  const out = [];
  for (const part of path.split("/")) {
    if (!part || part === ".") continue;
    if (part === "..") out.pop();
    else out.push(part);
  }
  return out.join("/");
}

function dirname(path) {
  const index = path.lastIndexOf("/");
  return index === -1 ? "" : path.substring(0, index);
}

function basename(path) {
  const index = path.lastIndexOf("/");
  return index === -1 ? path : path.substring(index + 1);
}

function joinZipPath(base, path) {
  if (!path) return normalizeZipPath(base);
  if (path.startsWith("/")) return normalizeZipPath(path.substring(1));
  return normalizeZipPath((base ? base + "/" : "") + path);
}

function relativeZipPath(fromFile, toPath) {
  const fromParts = dirname(fromFile).split("/").filter(Boolean);
  const toParts = toPath.split("/").filter(Boolean);
  while (fromParts.length && toParts.length && fromParts[0] === toParts[0]) {
    fromParts.shift();
    toParts.shift();
  }
  return [...fromParts.map(() => ".."), ...toParts].join("/") || basename(toPath);
}

function getElementsByLocalName(root, localName) {
  const seen = new Set();
  const elements = [];
  for (const element of [...root.getElementsByTagNameNS("*", localName), ...root.getElementsByTagName(localName)]) {
    if (seen.has(element)) continue;
    seen.add(element);
    elements.push(element);
  }
  return elements;
}

function parseOpfSpineHrefs(opfContent, opfPath) {
  const opfDir = opfPath.includes("/") ? opfPath.substring(0, opfPath.lastIndexOf("/")) : "";
  const opfBasePath = opfDir ? `${opfDir}/content.opf` : "content.opf";

  try {
    const doc = new DOMParser().parseFromString(opfContent, "application/xml");
    if (!doc.querySelector("parsererror")) {
      const manifest = {};
      for (const item of getElementsByLocalName(doc, "item")) {
        const id = item.getAttribute("id");
        const href = item.getAttribute("href");
        if (id && href) {
          manifest[id] = resolvePath(opfBasePath, decodeHref(href.split("#")[0]));
        }
      }

      const spineHrefs = [];
      for (const itemref of getElementsByLocalName(doc, "itemref")) {
        const idref = itemref.getAttribute("idref");
        if (idref && manifest[idref]) spineHrefs.push(manifest[idref]);
      }
      if (spineHrefs.length > 0) return spineHrefs;
    }
  } catch (e) {
    /* fall through to regex parser */
  }

  const manifest = {};
  const itemRegex = /<item\b[^>]*>/gi;
  let match;
  while ((match = itemRegex.exec(opfContent)) !== null) {
    const tag = match[0];
    const idMatch = tag.match(/\bid=["']([^"']+)["']/i);
    const hrefMatch = tag.match(/\bhref=["']([^"']+)["']/i);
    if (idMatch && hrefMatch) {
      manifest[idMatch[1]] = resolvePath(opfBasePath, decodeHref(hrefMatch[1].split("#")[0]));
    }
  }

  const spineHrefs = [];
  const itemrefRegex = /<itemref\b[^>]*idref=["']([^"']+)["'][^>]*>/gi;
  while ((match = itemrefRegex.exec(opfContent)) !== null) {
    if (manifest[match[1]]) spineHrefs.push(manifest[match[1]]);
  }
  return spineHrefs;
}

function extractLocationText(xhtmlContent) {
  try {
    const doc = new DOMParser().parseFromString(xhtmlContent, "application/xhtml+xml");
    if (!doc.querySelector("parsererror")) {
      doc.querySelectorAll("script,style,svg,metadata").forEach((el) => el.remove());
      return (doc.body || doc.documentElement).textContent || "";
    }
  } catch (e) {
    /* fall through to HTML parser */
  }

  try {
    const doc = new DOMParser().parseFromString(xhtmlContent, "text/html");
    doc.querySelectorAll("script,style,svg,metadata").forEach((el) => el.remove());
    return (doc.body || doc.documentElement).textContent || "";
  } catch (e) {
    return xhtmlContent
      .replace(/<script[\s\S]*?<\/script>/gi, " ")
      .replace(/<style[\s\S]*?<\/style>/gi, " ")
      .replace(/<svg[\s\S]*?<\/svg>/gi, " ")
      .replace(/<[^>]+>/g, " ");
  }
}

function countLocationWords(text) {
  const normalized = text.replace(/\s+/g, " ").trim();
  if (!normalized) return 0;
  try {
    return (normalized.match(/[\p{L}\p{N}]+(?:['’-][\p{L}\p{N}]+)*/gu) || []).length;
  } catch (e) {
    return (normalized.match(/[A-Za-z0-9]+(?:['-][A-Za-z0-9]+)*/g) || []).length;
  }
}

function countReferenceCharacters(text) {
  return Array.from(text.replace(/\s+/g, " ").trim()).length;
}

function utf8ByteLength(text) {
  return new TextEncoder().encode(text).length;
}

function isSafeSectionSplitElement(node) {
  if (!node || node.nodeType !== Node.ELEMENT_NODE) return true;
  return [
    "p",
    "div",
    "section",
    "article",
    "aside",
    "blockquote",
    "ul",
    "ol",
    "li",
    "h1",
    "h2",
    "h3",
    "h4",
    "h5",
    "h6",
    "hr",
  ].includes(localName(node));
}

function isHeadingElement(node) {
  return node && node.nodeType === Node.ELEMENT_NODE && /^h[1-6]$/i.test(localName(node));
}

function isIgnorableSectionSplitNode(node) {
  return node?.nodeType === Node.TEXT_NODE && !(node.textContent || "").trim();
}

function hasNaturalSectionPageBoundary(node) {
  if (!node || node.nodeType !== Node.ELEMENT_NODE) return false;
  const elements = [node, ...node.querySelectorAll("*")];
  return elements.some((element) => {
    const name = localName(element);
    if (name === "hr") return true;
    const epubType =
      element.getAttributeNS?.("http://www.idpf.org/2007/ops", "type") || element.getAttribute("epub:type") || "";
    const role = element.getAttribute("role") || "";
    const style = element.getAttribute("style") || "";
    return (
      /(^|\s)pagebreak(\s|$)/i.test(epubType) ||
      /(^|\s)doc-pagebreak(\s|$)/i.test(role) ||
      /(?:^|;)\s*(?:-epub-)?(?:page-break|break)-(?:before|after)\s*:\s*(?:always|page|left|right|recto|verso)\b/i.test(
        style,
      )
    );
  });
}

function isNaturalSectionSplitBoundary(previous, next) {
  return hasNaturalSectionPageBoundary(previous) || hasNaturalSectionPageBoundary(next) || isHeadingElement(next);
}

function findNaturalSectionSplitIndex(splitChildren, candidateIndex, current, currentBytes, serializer) {
  const byteLimit = Math.min(
    SECTION_SPLIT_HARD_BYTE_LIMIT,
    Math.max(currentBytes, SECTION_SPLIT_BYTE_THRESHOLD) + SECTION_SPLIT_NATURAL_BOUNDARY_LOOKAHEAD_BYTES,
  );
  let projectedBytes = currentBytes;
  for (let index = candidateIndex; index < splitChildren.length; index++) {
    if (index > candidateIndex) {
      projectedBytes += utf8ByteLength(serializer.serializeToString(splitChildren[index - 1]));
    }
    if (projectedBytes > byteLimit) break;
    const priorNodes = current.concat(splitChildren.slice(candidateIndex, index));
    const previous = [...priorNodes].reverse().find((node) => !isIgnorableSectionSplitNode(node));
    const next = splitChildren[index];
    const canBreakBefore =
      !!previous &&
      isSafeSectionSplitElement(next) &&
      !shouldKeepSectionSplitCluster(next) &&
      !isHeadingElement(previous);
    if (canBreakBefore && isNaturalSectionSplitBoundary(previous, next)) return index;
  }
  return candidateIndex;
}

function findNaturalSectionSplitOffsetInCurrent(current, serializer) {
  let trailingBytes = 0;
  for (let offset = current.length - 1; offset > 0; offset--) {
    trailingBytes += utf8ByteLength(serializer.serializeToString(current[offset]));
    if (trailingBytes > SECTION_SPLIT_NATURAL_BOUNDARY_LOOKAHEAD_BYTES) break;
    const previous = [...current.slice(0, offset)].reverse().find((node) => !isIgnorableSectionSplitNode(node));
    const next = current[offset];
    const canBreakBefore =
      !!previous &&
      isSafeSectionSplitElement(next) &&
      !shouldKeepSectionSplitCluster(next) &&
      !isHeadingElement(previous);
    if (canBreakBefore && isNaturalSectionSplitBoundary(previous, next)) return offset;
  }
  return -1;
}

function chunkHasReaderContent(nodes) {
  const visit = (node, hidden) => {
    if (node.nodeType === Node.TEXT_NODE) return !hidden && !!(node.textContent || "").trim();
    if (node.nodeType !== Node.ELEMENT_NODE) return false;
    const name = localName(node);
    const nextHidden =
      hidden || node.hasAttribute("data-AmznRemoved-M8") || ["script", "style", "svg", "metadata"].includes(name);
    if (nextHidden) return false;
    if (name === "img") return true;
    return Array.from(node.childNodes).some((child) => visit(child, nextHidden));
  };
  return nodes.some((node) => visit(node, false));
}

function shouldKeepSectionSplitCluster(node) {
  if (!node || node.nodeType !== Node.ELEMENT_NODE) return false;
  const name = localName(node);
  return ["table", "figure", "svg"].includes(name) || !!node.querySelector?.("table,figure,svg");
}

function isAtomicSectionSplitContainer(node) {
  return node?.nodeType === Node.ELEMENT_NODE && ["table", "figure", "svg"].includes(localName(node));
}

function findSectionSplitContainer(body) {
  let container = body;
  const childPath = [];
  while (true) {
    const children = Array.from(container.childNodes);
    const elementChildren = children
      .map((child, index) => ({ child, index }))
      .filter(({ child }) => child.nodeType === Node.ELEMENT_NODE);
    if (elementChildren.length >= 2) return { container, childPath };
    if (elementChildren.length !== 1 || isAtomicSectionSplitContainer(elementChildren[0].child)) return null;
    childPath.push(elementChildren[0].index);
    container = elementChildren[0].child;
  }
}

function makeSectionSplitPath(path, partIndex) {
  if (partIndex === 0) return path;
  const dot = path.lastIndexOf(".");
  const suffix = `__ci_section_${String(partIndex + 1).padStart(3, "0")}`;
  return dot > 0 ? `${path.substring(0, dot)}${suffix}${path.substring(dot)}` : `${path}${suffix}.xhtml`;
}

function splitBaseHref(href) {
  return href.replace(SECTION_SPLIT_SUFFIX_RE, "");
}

function readerSectionIdentity(content) {
  const doc = new DOMParser().parseFromString(content, "application/xhtml+xml");
  if (doc.querySelector("parsererror") || !doc.body) return null;
  const id = doc.body.getAttribute("id") || "";
  const title = doc.querySelector("title")?.textContent?.trim() || "";
  return id && title ? `${id}\u0000${title}` : null;
}

function hasReaderContent(content) {
  const doc = new DOMParser().parseFromString(content, "application/xhtml+xml");
  if (doc.querySelector("parsererror") || !doc.body) return true;
  const body = doc.body.cloneNode(true);
  body.querySelectorAll("script,style,svg,metadata,[data-AmznRemoved-M8]").forEach((node) => node.remove());
  return !!(body.textContent || "").trim() || !!body.querySelector("img");
}

function collapseReaderEmptySpineItems(xhtmlFiles, opfContent, opfPath) {
  const doc = new DOMParser().parseFromString(opfContent, "application/xml");
  const redirects = new Map();
  if (doc.querySelector("parsererror")) return { opfContent, redirects };
  const manifestById = new Map(
    Array.from(doc.getElementsByTagNameNS("*", "item")).map((item) => [item.getAttribute("id"), item]),
  );
  const spine = doc.getElementsByTagNameNS("*", "spine")[0];
  if (!spine) return { opfContent, redirects };
  const refs = Array.from(spine.getElementsByTagNameNS("*", "itemref"));
  for (let index = 0; index + 1 < refs.length; index++) {
    const item = manifestById.get(refs[index].getAttribute("idref"));
    const nextItem = manifestById.get(refs[index + 1].getAttribute("idref"));
    const href = item?.getAttribute("href");
    const nextHref = nextItem?.getAttribute("href");
    if (!href || !nextHref) continue;
    const path = resolvePath(opfPath, decodeHref(href.split("#")[0]));
    const nextPath = resolvePath(opfPath, decodeHref(nextHref.split("#")[0]));
    const content = xhtmlFiles[path];
    const nextContent = xhtmlFiles[nextPath];
    if (!content || !nextContent || hasReaderContent(content) || !hasReaderContent(nextContent)) continue;
    const identity = readerSectionIdentity(content);
    if (!identity || identity !== readerSectionIdentity(nextContent)) continue;
    redirects.set(path, nextPath);
    spine.removeChild(refs[index]);
  }
  return { opfContent: safeSerialize(doc, opfContent), redirects };
}

function rewriteCollapsedSpineReferences(content, sourcePath, redirects) {
  if (redirects.size === 0) return content;
  const doc = new DOMParser().parseFromString(content, "application/xml");
  if (doc.querySelector("parsererror")) return content;
  let changed = false;
  for (const element of Array.from(doc.getElementsByTagName("*"))) {
    for (const attribute of ["href", "src", "xlink:href"]) {
      const value = element.getAttribute(attribute);
      if (!value || value.startsWith("#") || /^[a-z][a-z0-9+.-]*:/i.test(value)) continue;
      const [href, fragment = ""] = value.split(/#(.*)/s, 2);
      const target = redirects.get(resolvePath(sourcePath, decodeHref(href)));
      if (!target) continue;
      element.setAttribute(attribute, `${relativeZipPath(sourcePath, target)}${fragment ? `#${fragment}` : ""}`);
      changed = true;
    }
  }
  return changed ? safeSerialize(doc, content) : content;
}

function rewriteSplitSectionReferences(content, sourcePath, anchorTargets) {
  if (anchorTargets.size === 0) return content;
  const doc = new DOMParser().parseFromString(content, "application/xml");
  if (doc.querySelector("parsererror")) return content;
  let changed = false;
  for (const element of Array.from(doc.getElementsByTagName("*"))) {
    for (const attribute of ["href", "xlink:href"]) {
      const value = element.getAttribute(attribute);
      if (!value || /^[a-z][a-z0-9+.-]*:/i.test(value)) continue;
      const [href, fragment = ""] = value.split(/#(.*)/s, 2);
      if (!fragment) continue;
      const targetPath = href ? resolvePath(sourcePath, decodeHref(href)) : sourcePath;
      const partPath = anchorTargets.get(targetPath)?.get(decodeHref(fragment));
      if (!partPath) continue;
      element.setAttribute(attribute, `${relativeZipPath(sourcePath, partPath)}#${fragment}`);
      changed = true;
    }
  }
  return changed ? safeSerialize(doc, content) : content;
}

function splitLongXhtmlSections(xhtmlFiles, opfContent, opfPath, enabled) {
  if (!enabled) return { files: xhtmlFiles, splitSections: {}, anchorTargets: new Map(), sourceSpineMap: null };

  const parser = new DOMParser();
  const serializer = new XMLSerializer();
  const out = {};
  const splitSections = {};
  const anchorTargets = new Map();
  const originalSpineHrefs = parseOpfSpineHrefs(opfContent, opfPath);
  const spinePaths = new Set(originalSpineHrefs);
  const sourceByHref = {};
  originalSpineHrefs.forEach((href, sourceSpineIndex) => {
    sourceByHref[href] = { sourceSpineIndex };
  });

  for (const [path, content] of Object.entries(xhtmlFiles)) {
    if (!spinePaths.has(path)) {
      out[path] = content;
      continue;
    }
    const totalWords = countLocationWords(extractLocationText(content));
    const totalBytes = utf8ByteLength(content);
    if (totalWords <= SECTION_SPLIT_WORD_THRESHOLD && totalBytes <= SECTION_SPLIT_BYTE_THRESHOLD) {
      out[path] = content;
      continue;
    }

    const doc = parser.parseFromString(content, "application/xhtml+xml");
    if (doc.querySelector("parsererror") || !doc.body) {
      out[path] = content;
      continue;
    }
    const splitContainer = findSectionSplitContainer(doc.body);
    if (!splitContainer) {
      out[path] = content;
      continue;
    }
    const splitChildren = Array.from(splitContainer.container.childNodes);
    const fixedBytes = Math.max(
      0,
      totalBytes - splitChildren.reduce((sum, child) => sum + utf8ByteLength(serializer.serializeToString(child)), 0),
    );

    const chunks = [];
    let current = [];
    let currentWords = 0;
    let currentBytes = fixedBytes;
    const flush = () => {
      if (current.length === 0) return;
      chunks.push(current);
      current = [];
      currentWords = 0;
      currentBytes = fixedBytes;
    };

    for (let childIndex = 0; childIndex < splitChildren.length; childIndex++) {
      const child = splitChildren[childIndex];
      const childWords = countLocationWords(child.textContent || "");
      const childBytes = utf8ByteLength(serializer.serializeToString(child));
      const lastContentNode = [...current].reverse().find((node) => !isIgnorableSectionSplitNode(node));
      const wouldExceed =
        !!lastContentNode &&
        (currentWords + childWords > SECTION_SPLIT_WORD_THRESHOLD ||
          currentBytes + childBytes > SECTION_SPLIT_BYTE_THRESHOLD);
      const canBreakBefore =
        !!lastContentNode &&
        isSafeSectionSplitElement(child) &&
        !shouldKeepSectionSplitCluster(child) &&
        !isHeadingElement(lastContentNode);
      const naturalSplitOffset =
        wouldExceed && currentBytes <= SECTION_SPLIT_HARD_BYTE_LIMIT
          ? findNaturalSectionSplitOffsetInCurrent(current, serializer)
          : -1;
      if (naturalSplitOffset > 0) {
        const completed = current.splice(0, naturalSplitOffset);
        chunks.push(completed);
        currentWords = current.reduce((sum, node) => sum + countLocationWords(node.textContent || ""), 0);
        currentBytes =
          fixedBytes + current.reduce((sum, node) => sum + utf8ByteLength(serializer.serializeToString(node)), 0);
      } else if (wouldExceed && canBreakBefore) {
        const naturalSplitIndex = findNaturalSectionSplitIndex(
          splitChildren,
          childIndex,
          current,
          currentBytes,
          serializer,
        );
        if (naturalSplitIndex > childIndex) {
          for (let index = childIndex; index < naturalSplitIndex; index++) {
            const node = splitChildren[index];
            current.push(node);
            currentWords += countLocationWords(node.textContent || "");
            currentBytes += utf8ByteLength(serializer.serializeToString(node));
          }
          flush();
          childIndex = naturalSplitIndex - 1;
          continue;
        }
        flush();
      }

      current.push(child);
      currentWords += childWords;
      currentBytes += childBytes;

      const canBreakAfter =
        !isIgnorableSectionSplitNode(child) &&
        isSafeSectionSplitElement(child) &&
        !shouldKeepSectionSplitCluster(child) &&
        !isHeadingElement(child);
      if (currentBytes >= SECTION_SPLIT_HARD_BYTE_LIMIT && canBreakAfter) flush();
    }
    flush();
    const mergedChunks = [];
    let pending = [];
    for (const chunk of chunks) {
      if (chunkHasReaderContent(chunk)) {
        mergedChunks.push([...pending, ...chunk]);
        pending = [];
      } else {
        pending.push(...chunk);
      }
    }
    if (pending.length && mergedChunks.length) mergedChunks[mergedChunks.length - 1].push(...pending);
    chunks.splice(0, chunks.length, ...mergedChunks);

    if (chunks.length < 2) {
      out[path] = content;
      continue;
    }

    splitSections[path] = [];
    const sectionAnchors = new Map();
    anchorTargets.set(path, sectionAnchors);
    const tagOffsets = new Map();
    chunks.forEach((chunk, partIndex) => {
      const partPath = makeSectionSplitPath(path, partIndex);
      const partDoc = doc.cloneNode(true);
      let partContainer = partDoc.body;
      for (const childIndex of splitContainer.childPath) partContainer = partContainer.childNodes[childIndex];
      while (partContainer.firstChild) partContainer.removeChild(partContainer.firstChild);
      for (const node of chunk) partContainer.appendChild(partDoc.importNode(node, true));
      for (const element of Array.from(partDoc.getElementsByTagName("*"))) {
        const anchor = element.getAttribute("id") || (localName(element) === "a" ? element.getAttribute("name") : null);
        if (anchor && !sectionAnchors.has(anchor)) sectionAnchors.set(anchor, partPath);
      }
      out[partPath] = safeSerialize(partDoc, content);
      splitSections[path].push(partPath);
      const rangesByName = new Map();
      for (const node of chunk) {
        if (node.nodeType !== Node.ELEMENT_NODE) continue;
        const name = localName(node);
        const range = rangesByName.get(name) || { name, offset: tagOffsets.get(name) || 0, count: 0 };
        range.count++;
        rangesByName.set(name, range);
        tagOffsets.set(name, (tagOffsets.get(name) || 0) + 1);
      }
      sourceByHref[partPath] = {
        sourceSpineIndex: originalSpineHrefs.indexOf(path),
        containerDepth: splitContainer.childPath.length,
        childRanges: Array.from(rangesByName.values()),
      };
    });
  }

  const hasSplits = Object.keys(splitSections).length > 0;
  return {
    files: out,
    splitSections,
    anchorTargets,
    sourceSpineMap: hasSplits ? { version: 1, spineCount: originalSpineHrefs.length, sourceByHref } : null,
  };
}

function addSplitSectionsToOpf(opfContent, opfPath, splitSections) {
  if (Object.keys(splitSections).length === 0) return opfContent;

  const doc = new DOMParser().parseFromString(opfContent, "application/xml");
  if (doc.querySelector("parsererror")) return opfContent;
  const manifest = doc.getElementsByTagNameNS("*", "manifest")[0];
  const spine = doc.getElementsByTagNameNS("*", "spine")[0];
  if (!manifest || !spine) return opfContent;

  const byPath = new Map();
  const items = Array.from(doc.getElementsByTagNameNS("*", "item"));
  for (const item of items) {
    const id = item.getAttribute("id");
    const href = item.getAttribute("href");
    if (id && href) byPath.set(resolvePath(opfPath, decodeHref(href.split("#")[0])), { id, item });
  }
  const usedIds = new Set(items.map((item) => item.getAttribute("id") || ""));

  for (const [originalPath, parts] of Object.entries(splitSections)) {
    const original = byPath.get(originalPath);
    if (!original || parts.length < 2) continue;

    const addedIds = [];
    for (let index = 1; index < parts.length; index++) {
      let id = `${original.id}-ci-${index + 1}`;
      while (usedIds.has(id)) id += "x";
      usedIds.add(id);

      const item = doc.createElementNS(manifest.namespaceURI || OPF_NS, "item");
      item.setAttribute("id", id);
      item.setAttribute("href", relativeZipPath(opfPath, parts[index]));
      item.setAttribute("media-type", "application/xhtml+xml");
      manifest.appendChild(item);
      addedIds.push(id);
    }

    const itemref = Array.from(doc.getElementsByTagNameNS("*", "itemref")).find(
      (ref) => ref.getAttribute("idref") === original.id,
    );
    if (!itemref) continue;
    let insertAfter = itemref;
    for (const id of addedIds) {
      const ref = doc.createElementNS(spine.namespaceURI || OPF_NS, "itemref");
      ref.setAttribute("idref", id);
      if (insertAfter.nextSibling) spine.insertBefore(ref, insertAfter.nextSibling);
      else spine.appendChild(ref);
      insertAfter = ref;
    }
  }

  return safeSerialize(doc, opfContent);
}

function resolveXhtmlContentForLocation(path, xhtmlFiles) {
  if (xhtmlFiles[path]) return xhtmlFiles[path];
  const decoded = decodeHref(path);
  if (xhtmlFiles[decoded]) return xhtmlFiles[decoded];
  const filename = decoded.split("/").pop();
  const match = Object.entries(xhtmlFiles).find(
    ([candidate]) => candidate === decoded || candidate.endsWith("/" + filename),
  );
  return match ? match[1] : null;
}

function normalizedReferenceCharactersPerPage(value) {
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) ? Math.max(1, Math.min(10000, parsed)) : X_DEFAULT_REFERENCE_CHARACTERS_PER_PAGE;
}

function buildXLocationManifest(
  opfContent,
  opfPath,
  xhtmlFiles,
  charactersPerReferencePage = X_DEFAULT_REFERENCE_CHARACTERS_PER_PAGE,
  sourceSpineMap = null,
) {
  charactersPerReferencePage = normalizedReferenceCharactersPerPage(charactersPerReferencePage);
  const spineHrefs = parseOpfSpineHrefs(opfContent, opfPath);
  if (spineHrefs.length === 0) return null;

  const spine = [];
  const splitBases = new Set(spineHrefs.map(splitBaseHref).filter((base, index) => base !== spineHrefs[index]));
  const groupByBase = new Map(
    Array.from(splitBases)
      .sort()
      .map((base, index) => [base, index]),
  );
  const chapterGroups = new Map();
  let totalWords = 0;
  let totalCharacters = 0;
  let nextLocation = 1;

  for (let index = 0; index < spineHrefs.length; index++) {
    const href = spineHrefs[index];
    const content = resolveXhtmlContentForLocation(href, xhtmlFiles);
    const visibleText = content ? extractLocationText(content) : "";
    const wordCount = countLocationWords(visibleText);
    const characterCount = countReferenceCharacters(visibleText);
    const locationCount = Math.ceil(wordCount / X_LOCATION_WORDS_PER_UNIT);
    const startLocation = locationCount > 0 ? nextLocation : 0;
    const endLocation = locationCount > 0 ? nextLocation + locationCount - 1 : 0;
    const startReferencePage = characterCount > 0 ? Math.floor(totalCharacters / charactersPerReferencePage) + 1 : 0;
    const endReferencePage =
      characterCount > 0 ? Math.ceil((totalCharacters + characterCount) / charactersPerReferencePage) : 0;

    const spineEntry = {
      index,
      href,
      wordStart: totalWords,
      wordCount,
      characterStart: totalCharacters,
      characterCount,
      startLocation,
      endLocation,
      startReferencePage,
      endReferencePage,
    };
    const baseHref = splitBaseHref(href);
    const chapterGroup = groupByBase.get(baseHref);
    if (chapterGroup !== undefined) {
      spineEntry.chapterGroup = chapterGroup;
      const group = chapterGroups.get(chapterGroup) || {
        index: chapterGroup,
        title: basename(baseHref).replace(/\.[^.]+$/, ""),
        firstSpineIndex: index,
        lastSpineIndex: index,
        startLocation,
        endLocation,
        wordStart: totalWords,
        wordCount: 0,
        characterStart: totalCharacters,
        characterCount: 0,
        referencePageStart: startReferencePage,
        referencePageEnd: endReferencePage,
      };
      group.firstSpineIndex = Math.min(group.firstSpineIndex, index);
      group.lastSpineIndex = Math.max(group.lastSpineIndex, index);
      if (startLocation > 0 && (group.startLocation === 0 || startLocation < group.startLocation)) {
        group.startLocation = startLocation;
      }
      group.endLocation = Math.max(group.endLocation, endLocation);
      group.wordCount += wordCount;
      group.characterCount += characterCount;
      if (startReferencePage > 0 && (group.referencePageStart === 0 || startReferencePage < group.referencePageStart)) {
        group.referencePageStart = startReferencePage;
      }
      group.referencePageEnd = Math.max(group.referencePageEnd, endReferencePage);
      chapterGroups.set(chapterGroup, group);
    }
    spine.push(spineEntry);

    totalWords += wordCount;
    totalCharacters += characterCount;
    nextLocation += locationCount;
  }

  const manifest = {
    format: "x-locations",
    version: 1,
    generator: "crossink-web-uploader",
    unit: "word",
    referencePageUnit: "character",
    wordsPerLocation: X_LOCATION_WORDS_PER_UNIT,
    charactersPerReferencePage,
    totalWords,
    totalCharacters,
    totalLocations: Math.max(0, nextLocation - 1),
    totalReferencePages: Math.ceil(totalCharacters / charactersPerReferencePage),
    spine,
  };
  if (chapterGroups.size > 0) {
    manifest.chapterGroups = Array.from(chapterGroups.values()).sort((a, b) => a.index - b.index);
  }
  if (sourceSpineMap) {
    const mappedSpine = spineHrefs.map((href, index) => ({ index, ...sourceSpineMap.sourceByHref[href] }));
    if (mappedSpine.every((entry) => Number.isInteger(entry.sourceSpineIndex))) {
      manifest.sourceSpineMap = {
        version: sourceSpineMap.version,
        spineCount: sourceSpineMap.spineCount,
        spine: mappedSpine,
      };
    }
  }
  return manifest;
}

/**
 * Extract main identifier from OPF for NCX sync. DOMParser with regex fallback.
 */
function extractIdentifier(opfContent) {
  let mainIdentifier = null;
  try {
    const doc = new DOMParser().parseFromString(opfContent, "application/xml");
    if (!doc.querySelector("parsererror")) {
      const pkg = doc.getElementsByTagNameNS("*", "package")[0];
      const uid = pkg ? pkg.getAttribute("unique-identifier") : null;
      if (uid) {
        const el = [...doc.getElementsByTagNameNS("*", "identifier")].find((e) => e.getAttribute("id") === uid);
        if (el) mainIdentifier = (el.textContent || "").trim();
      }
      if (!mainIdentifier) {
        const el = doc.getElementsByTagNameNS("*", "identifier")[0];
        if (el) mainIdentifier = (el.textContent || "").trim();
      }
    }
  } catch (e) {
    /* fall through to regex */
  }
  if (!mainIdentifier) {
    const uniqueIdMatch = opfContent.match(/<(?:\w+:)?package[^>]*unique-identifier=["']([^"']+)["']/i);
    if (uniqueIdMatch) {
      const idRegex = new RegExp(`<dc:identifier[^>]*id=["']${uniqueIdMatch[1]}["'][^>]*>([^<]+)</dc:identifier>`, "i");
      const idMatch = opfContent.match(idRegex);
      if (idMatch) mainIdentifier = idMatch[1].trim();
    }
    if (!mainIdentifier) {
      const firstIdMatch = opfContent.match(/<dc:identifier[^>]*>([^<]+)</i);
      if (firstIdMatch) mainIdentifier = firstIdMatch[1].trim();
    }
  }
  return mainIdentifier;
}

/**
 * Sync NCX dtb:uid with the given identifier. DOMParser with regex fallback.
 */
function syncNCXIdentifier(ncxText, mainIdentifier) {
  if (!mainIdentifier) return ncxText;
  let t = ncxText;
  try {
    const doc = new DOMParser().parseFromString(t, "application/xml");
    if (!doc.querySelector("parsererror")) {
      const meta = [...doc.getElementsByTagNameNS("*", "meta")].find((m) => m.getAttribute("name") === "dtb:uid");
      if (meta) {
        meta.setAttribute("content", mainIdentifier);
        t = safeSerialize(doc, ncxText);
      }
    }
  } catch (e) {
    t = t.replace(
      /<meta\s+name=["']dtb:uid["']\s+content=["'][^"']*["']\s*\/?>/gi,
      `<meta name="dtb:uid" content="${xmlEscape(mainIdentifier)}"/>`,
    );
  }
  return t;
}

/**
 * Fix OPF content: fix media-types, strip svg properties,
 * update split image manifest entries, ensure cover meta.
 * DOMParser with regex fallback.
 */
function fixOPF(opfText, opfOriginal, opfDir, splitImages = {}) {
  let t = opfText;

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(t, "application/xml");
    if (doc.querySelector("parsererror")) throw new Error("OPF parse failed");

    const items = [...doc.getElementsByTagNameNS("*", "item")];
    const manifestEl = doc.getElementsByTagNameNS("*", "manifest")[0];

    // Fix media-types for converted images
    for (const item of items) {
      const href = item.getAttribute("href") || "";
      const type = item.getAttribute("media-type") || "";
      if (href.endsWith(".jpg") && type.match(/^image\/(png|gif|webp|bmp|svg\+xml)$/)) {
        item.setAttribute("media-type", "image/jpeg");
      }
    }

    // Remove 'svg' from properties
    for (const item of items) {
      const props = item.getAttribute("properties") || "";
      if (props.includes("svg")) {
        const newProps = props
          .split(/\s+/)
          .filter((p) => p !== "svg")
          .join(" ")
          .trim();
        if (newProps) item.setAttribute("properties", newProps);
        else item.removeAttribute("properties");
      }
    }

    // Update split image hrefs and add manifest entries for parts
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + "/") ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg|svg)$/i, ".jpg");
      const part1Href = origHrefJpg.replace(/\.jpg$/i, "_part1.jpg");

      for (const item of items) {
        const h = item.getAttribute("href") || "";
        if (h === origHref || h === origHrefJpg || decodeHref(h) === origHref || decodeHref(h) === origHrefJpg) {
          item.setAttribute("href", part1Href);
          break;
        }
      }

      if (manifestEl) {
        const ns = manifestEl.namespaceURI || "http://www.idpf.org/2007/opf";
        for (let j = 1; j < parts.length; j++) {
          const p = parts[j];
          const href = opfDir && p.path.startsWith(opfDir + "/") ? p.path.substring(opfDir.length + 1) : p.path;
          const newItem = doc.createElementNS(ns, "item");
          newItem.setAttribute("id", `img-${p.id}`);
          newItem.setAttribute("href", href);
          newItem.setAttribute("media-type", "image/jpeg");
          manifestEl.appendChild(newItem);
        }
      }
    }

    t = safeSerialize(doc, opfOriginal);
  } catch (e) {
    // Regex fallback
    t = t.replace(
      /(<(?:\w+:)?item\b[^>]*href="[^"]+\.jpg"[^>]*)media-type="image\/(png|gif|webp|bmp|svg\+xml)"/g,
      '$1media-type="image/jpeg"',
    );
    t = t.replace(
      /(<(?:\w+:)?item\b[^>]*)media-type="image\/(png|gif|webp|bmp|svg\+xml)"([^>]*href="[^"]+\.jpg")/g,
      '$1media-type="image/jpeg"$3',
    );
    t = t.replace(/\s+svg(?=["'\s>])/g, "");
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + "/") ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg|svg)$/i, ".jpg");
      const part1Href = origHrefJpg.replace(/\.jpg$/i, "_part1.jpg");
      const origImgRegex = new RegExp(`(href=["'])(${escapeRegex(origHref)}|${escapeRegex(origHrefJpg)})(["'])`, "gi");
      t = t.replace(origImgRegex, `$1${part1Href}$3`);
      let adds = "";
      for (let j = 1; j < parts.length; j++) {
        const p = parts[j];
        const href = opfDir && p.path.startsWith(opfDir + "/") ? p.path.substring(opfDir.length + 1) : p.path;
        adds += `<item id="img-${xmlEscape(p.id)}" href="${xmlEscape(href)}" media-type="image/jpeg"/>\n`;
      }
      if (adds && t.includes("</manifest>")) t = t.replace("</manifest>", adds + "</manifest>");
    }
  }

  // Ensure cover meta
  const cm = ensureCoverMeta(t);
  if (cm.fixed) t = cm.o;

  return t;
}

// Fix SVG cover - converts SVG-wrapped covers to plain HTML img tags
function fixSvgCover(content) {
  const hasSvg = content.includes("<svg") || content.includes("<svg:");
  if (!hasSvg || !content.includes("xlink:href")) return { c: content, fixed: false, count: 0 };
  if (
    !content.includes("calibre:cover") &&
    !content.includes('name="cover"') &&
    !content.includes("<title>Cover</title>")
  )
    return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, "application/xhtml+xml");

    if (doc.querySelector("parsererror")) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      return {
        c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`,
        fixed: true,
        count: 1,
      };
    }

    // Find SVG elements - check both standard and namespaced variants
    let imgHref = null;
    const svgNS = "http://www.w3.org/2000/svg";
    const xlinkNS = "http://www.w3.org/1999/xlink";

    // Try to find all SVG elements
    const svgs = [
      ...doc.getElementsByTagName("svg"),
      ...doc.getElementsByTagNameNS(svgNS, "svg"),
      ...doc.getElementsByTagName("svg:svg"),
    ];

    for (const svg of svgs) {
      // Find image element inside - try all variants
      const imageEl =
        svg.getElementsByTagName("image")[0] ||
        svg.getElementsByTagNameNS(svgNS, "image")[0] ||
        svg.getElementsByTagName("svg:image")[0];

      if (imageEl) {
        imgHref =
          imageEl.getAttributeNS(xlinkNS, "href") || imageEl.getAttribute("xlink:href") || imageEl.getAttribute("href");
        if (imgHref) break;
      }
    }

    if (!imgHref) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      imgHref = m[1];
    }

    return {
      c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${imgHref}"/></section></body>
</html>`,
      fixed: true,
      count: 1,
    };
  } catch (e) {
    // Fallback to regex
    const m = content.match(/xlink:href=["']([^"']+)["']/);
    if (!m) return { c: content, fixed: false, count: 0 };
    return {
      c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`,
      fixed: true,
      count: 1,
    };
  }
}

// Fix SVG-wrapped images - unwrap SVG and replace with plain img
function fixSvgWrappedImages(content) {
  const hasSvg = content.includes("<svg") || content.includes("<svg:");
  if (!hasSvg || !content.includes("xlink:href")) return { c: content, fixed: false, count: 0 };

  try {
    const whitespaceGuard = protectWhitespaceOnlyTextNodes(content);
    const parser = new DOMParser();
    const doc = parser.parseFromString(whitespaceGuard.content, "application/xhtml+xml");

    if (doc.querySelector("parsererror")) {
      // Fallback to regex
      let fixedCount = 0;
      const svgImageRegex =
        /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
      const newContent = content.replace(svgImageRegex, (match, href) => {
        fixedCount++;
        return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`;
      });
      return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
    }

    const svgNS = "http://www.w3.org/2000/svg";
    const xlinkNS = "http://www.w3.org/1999/xlink";

    const svgElements = [...doc.querySelectorAll("svg"), ...doc.getElementsByTagNameNS(svgNS, "svg")];
    const uniqueSvgs = [...new Set(svgElements)];
    let fixedCount = 0;

    for (const svg of uniqueSvgs) {
      const imageEl =
        svg.querySelector("image[*|href]") ||
        svg.getElementsByTagNameNS(svgNS, "image")[0] ||
        svg.getElementsByTagNameNS("*", "image")[0];
      if (!imageEl) continue;
      const href =
        imageEl.getAttributeNS(xlinkNS, "href") || imageEl.getAttribute("xlink:href") || imageEl.getAttribute("href");
      if (!href) continue;
      const width = imageEl.getAttribute("width") || svg.getAttribute("width");
      const height = imageEl.getAttribute("height") || svg.getAttribute("height");
      const img = doc.createElementNS("http://www.w3.org/1999/xhtml", "img");
      img.setAttribute("src", href);
      img.setAttribute("alt", "");
      img.setAttribute("style", "max-width:100%;height:auto");
      if (width) img.setAttribute("width", width);
      if (height) img.setAttribute("height", height);
      svg.parentNode.replaceChild(img, svg);
      fixedCount++;
    }

    if (fixedCount === 0) return { c: content, fixed: false, count: 0 };
    return { c: whitespaceGuard.restore(safeSerialize(doc, whitespaceGuard.content)), fixed: true, count: fixedCount };
  } catch (e) {
    // Fallback to regex
    let fixedCount = 0;
    const svgImageRegex =
      /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
    const newContent = content.replace(svgImageRegex, (match, href) => {
      fixedCount++;
      return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`;
    });
    return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
  }
}

// Ensure cover meta tag exists in OPF — DOMParser with regex fallback
function ensureCoverMeta(opfString) {
  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(opfString, "application/xml");
    if (doc.querySelector("parsererror")) throw new Error("Parse failed");

    // Find cover image id: properties="cover-image", or id/href containing "cover"
    let coverId = null;
    const items = [...doc.getElementsByTagNameNS("*", "item")];
    for (const item of items) {
      const props = item.getAttribute("properties") || "";
      const id = item.getAttribute("id") || "";
      const type = item.getAttribute("media-type") || "";
      if (!type.startsWith("image/")) continue;
      if (props.includes("cover-image")) {
        coverId = id;
        break;
      }
    }
    if (!coverId) {
      for (const item of items) {
        const id = item.getAttribute("id") || "";
        const href = item.getAttribute("href") || "";
        const type = item.getAttribute("media-type") || "";
        if (!type.startsWith("image/")) continue;
        if (id.toLowerCase().includes("cover") || href.toLowerCase().includes("cover")) {
          coverId = id;
          break;
        }
      }
    }
    if (!coverId) return { o: opfString, fixed: false };

    // Find or create <meta name="cover" content="..."/>
    const metas = [...doc.getElementsByTagNameNS("*", "meta")];
    const coverMeta = metas.find((m) => m.getAttribute("name") === "cover");
    if (coverMeta) {
      if (coverMeta.getAttribute("content") === coverId) return { o: opfString, fixed: false };
      coverMeta.setAttribute("content", coverId);
    } else {
      const metadata = doc.getElementsByTagNameNS("*", "metadata")[0];
      if (!metadata) return { o: opfString, fixed: false };
      const ns = metadata.namespaceURI || "http://www.idpf.org/2007/opf";
      const newMeta = doc.createElementNS(ns, "meta");
      newMeta.setAttribute("name", "cover");
      newMeta.setAttribute("content", coverId);
      metadata.appendChild(newMeta);
    }
    return { o: safeSerialize(doc, opfString), fixed: true };
  } catch (e) {
    // Regex fallback
    return ensureCoverMetaRegex(opfString);
  }
}

function ensureCoverMetaRegex(o) {
  let coverId = null,
    m;
  if (!coverId && (m = o.match(/<\w+:?item[^>]+id="([^"]+)"[^>]+properties="[^"]*cover-image[^"]*"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id="([^"]+)"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]+)"[^>]*href="[^"]*cover[^"]*"[^>]*media-type="image\//i)))
    coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*href="[^"]*cover[^"]*"[^>]*id="([^"]+)"[^>]*media-type="image\//i)))
    coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]*cover[^"]*)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*media-type="image\/[^"]*"[^>]*id="([^"]*cover[^"]*)"/i))) coverId = m[1];
  if (!coverId) return { o, fixed: false };
  const metaMatch =
    o.match(/<\w+:?meta\s+name=["']cover["']\s+content=["']([^"']+)["']/i) ||
    o.match(/<\w+:?meta\s+content=["']([^"']+)["']\s+name=["']cover["']/i);
  if (metaMatch) {
    if (metaMatch[1] === coverId && !metaMatch[1].includes("/")) return { o, fixed: false };
    const esc = xmlEscape(coverId);
    o = o.replace(
      /<\w+:?meta\s+name=["']cover["']\s+content=["'][^"']+["']\s*\/?>/gi,
      `<meta name="cover" content="${esc}" />`,
    );
    o = o.replace(
      /<\w+:?meta\s+content=["'][^"']+["']\s+name=["']cover["']\s*\/?>/gi,
      `<meta name="cover" content="${esc}" />`,
    );
    return { o, fixed: true };
  }
  const idx = o.indexOf("</metadata>");
  if (idx !== -1)
    return {
      o:
        o.substring(0, idx) +
        `    <meta name="cover" content="${xmlEscape(coverId)}"/>\n  </metadata>` +
        o.substring(idx + 11),
      fixed: true,
    };
  return { o, fixed: false };
}

function flattenCanvasAlpha(ctx, width, height) {
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    if (data[i + 3] === 255) continue;
    const a = data[i + 3] / 255;
    data[i] = Math.round(data[i] * a + 255 * (1 - a));
    data[i + 1] = Math.round(data[i + 1] * a + 255 * (1 - a));
    data[i + 2] = Math.round(data[i + 2] * a + 255 * (1 - a));
    data[i + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
}

// Apply grayscale to canvas image data
function applyGrayscale(ctx, width, height) {
  flattenCanvasAlpha(ctx, width, height);
  if (!ENABLE_GRAYSCALE) return;
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    const gray = Math.round(data[i] * 0.299 + data[i + 1] * 0.587 + data[i + 2] * 0.114);
    data[i] = gray;
    data[i + 1] = gray;
    data[i + 2] = gray;
  }
  ctx.putImageData(imageData, 0, 0);
}

async function findEpubCoverImagePaths(zip) {
  const opfPath = await findOPFPath(zip);
  const entry = opfPath && zip.files[opfPath];
  if (!entry) return new Set();
  const doc = new DOMParser().parseFromString(await safeReadText(entry), "application/xml");
  if (doc.getElementsByTagName("parsererror").length) return new Set();

  const coverId = Array.from(doc.getElementsByTagName("meta"))
    .find((meta) => meta.getAttribute("name") === "cover")
    ?.getAttribute("content");
  const items = Array.from(doc.getElementsByTagName("item"));
  const coverItem = (coverId && items.find((item) => item.getAttribute("id") === coverId)) ||
    items.find((item) => (item.getAttribute("properties") || "").split(/\s+/).includes("cover-image")) ||
    items.find((item) => {
    const properties = item.getAttribute("properties") || "";
    const id = item.getAttribute("id") || "";
    const href = item.getAttribute("href") || "";
    return !properties.includes("cover-image") &&
      (item.getAttribute("media-type") || "").startsWith("image/") && /cover/i.test(`${id} ${href}`);
    });
  const href = coverItem?.getAttribute("href");
  return href ? new Set([resolvePath(opfPath, decodeHref(href.split("#")[0]))]) : new Set();
}

// Process single image - returns array of {data, suffix} objects
const IMAGE_LOAD_TIMEOUT_MS = 30000; // 30 second timeout for image loading
async function processImage(data, imageState = 0, imagePath = "", preserveColor = false) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(new Blob([data], { type: imageMimeType(imagePath) }));
    const img = new Image();
    const origSize = data.byteLength;
    // Set up timeout to handle cases where image never loads
    const timeoutId = setTimeout(() => {
      URL.revokeObjectURL(url);
      reject(new Error("Image load timeout"));
    }, IMAGE_LOAD_TIMEOUT_MS);

    img.onload = async () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      const origW = img.width,
        origH = img.height;

      if (preserveColor) {
        const scale = Math.min(1, COVER_MAX_WIDTH / origW, COVER_MAX_HEIGHT / origH);
        const width = Math.max(1, Math.round(origW * scale));
        const height = Math.max(1, Math.round(origH * scale));
        const canvas = document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext("2d");
        context.imageSmoothingEnabled = true;
        context.imageSmoothingQuality = "high";
        context.fillStyle = "#FFF";
        context.fillRect(0, 0, width, height);
        context.drawImage(img, 0, 0, width, height);
        const blob = await new Promise((res) => canvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
        const output = await blob.arrayBuffer();
        resolve({
          parts: [{ data: output, suffix: "", width, height, size: output.byteLength }],
          meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: width, finalH: height, finalSize: output.byteLength, imageState: 0 },
        });
        return;
      }

      // imageState: 0=Normal, 1=H-Split (CW/CCW), 2=V-Split, 3=Rotate & Fit
      // ========================================================================
      // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
      // Step 1: Scale WIDTH to 800px (keep aspect ratio)
      // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
      // Step 3: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      if (imageState === 1) {
        // Step 1: Scale WIDTH to 800 (this is the key difference!)
        const scale = MAX_HEIGHT / origW; // 800 / origW
        const scaledW = MAX_HEIGHT; // 800
        const scaledH = Math.round(origH * scale);

        const scaledCanvas = document.createElement("canvas");
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext("2d");
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = "high";
        scaledCtx.fillStyle = "#FFF";
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(img, 0, 0, origW, origH, 0, 0, scaledW, scaledH);

        // Step 2: Rotate 90° CW or CCW
        const rotW = scaledH;
        const rotH = scaledW; // 800

        const rotCanvas = document.createElement("canvas");
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext("2d");
        rotCtx.fillStyle = "#FFF";
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === "right";
        if (isClockwise) {
          // Rotate 90° CW
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          // Rotate 90° CCW
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(scaledCanvas, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform
        applyGrayscale(rotCtx, rotW, rotH);

        // Step 3: If WIDTH > 480, split vertically
        if (rotW <= MAX_WIDTH) {
          const blob = await new Promise((res) => rotCanvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: "", width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: false,
              rotated: true,
              finalW: rotW,
              finalH: rotH,
              finalSize: arrBuf.byteLength,
              imageState: 1,
            },
          });
        } else {
          // Split by WIDTH (vertical cuts) - from RIGHT to LEFT for CW, LEFT to RIGHT for CCW
          const parts = [];
          const maxW = MAX_WIDTH; // 480

          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100)); // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((rotW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((rotW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x;
            if (isClockwise) {
              // CW: right to left - start from right edge
              x = rotW - maxW - i * step;
            } else {
              // CCW: left to right - start from left edge
              x = i * step;
            }
            // Clamp to valid range
            x = Math.max(0, Math.min(x, rotW - maxW));
            positions.push(x);
          }

          // Ensure first and last positions are at edges
          if (isClockwise) {
            positions[0] = rotW - maxW; // First part at right edge
            positions[numParts - 1] = 0; // Last part at left edge
          } else {
            positions[0] = 0; // First part at left edge
            positions[numParts - 1] = rotW - maxW; // Last part at right edge
          }

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement("canvas");
            partCanvas.width = partW;
            partCanvas.height = rotH;
            const partCtx = partCanvas.getContext("2d");
            // Clear canvas first
            partCtx.fillStyle = "#FFFFFF";
            partCtx.fillRect(0, 0, partW, rotH);
            // Draw the slice
            partCtx.drawImage(rotCanvas, x, 0, partW, rotH, 0, 0, partW, rotH);

            const blob = await new Promise((res) => partCanvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: rotH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: true,
              splitCount: numParts,
              rotated: true,
              finalW: parts[0].width,
              finalH: parts[0].height,
              finalSize: totalSize,
              imageState: 1,
            },
          });
        }
      }
      // ========================================================================
      // STATE 2: V-Split (Vertical Split, no rotation)
      // Step 1: Scale HEIGHT to 800px (up or down)
      // Step 2: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      else if (imageState === 2) {
        // ALWAYS scale height to 800 (up or down)
        const scale = MAX_HEIGHT / origH; // 800 / origH
        const scaledW = Math.round(origW * scale);
        const scaledH = MAX_HEIGHT; // Always 800

        const scaledCanvas = document.createElement("canvas");
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext("2d");
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = "high";
        scaledCtx.fillStyle = "#FFF";
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(img, 0, 0, origW, origH, 0, 0, scaledW, scaledH);
        applyGrayscale(scaledCtx, scaledW, scaledH);

        // Check if split needed
        if (scaledW <= MAX_WIDTH) {
          const blob = await new Promise((res) => scaledCanvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: "", width: scaledW, height: scaledH, size: arrBuf.byteLength }],
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: false,
              rotated: false,
              finalW: scaledW,
              finalH: scaledH,
              finalSize: arrBuf.byteLength,
              imageState: 2,
            },
          });
        } else {
          // Split by WIDTH (vertical cuts) - LEFT to RIGHT (natural reading order)
          const parts = [];
          const maxW = MAX_WIDTH;

          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100)); // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((scaledW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((scaledW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x = i * step;
            // Clamp to valid range
            x = Math.max(0, Math.min(x, scaledW - maxW));
            positions.push(x);
          }
          // Ensure last position is at right edge
          positions[0] = 0;
          positions[numParts - 1] = scaledW - maxW;

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement("canvas");
            partCanvas.width = partW;
            partCanvas.height = scaledH;
            const partCtx = partCanvas.getContext("2d");
            // Clear canvas first
            partCtx.fillStyle = "#FFFFFF";
            partCtx.fillRect(0, 0, partW, scaledH);
            // Draw the slice
            partCtx.drawImage(scaledCanvas, x, 0, partW, scaledH, 0, 0, partW, scaledH);

            const blob = await new Promise((res) => partCanvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({
              data: arrBuf,
              suffix: `_part${i + 1}`,
              width: partW,
              height: scaledH,
              size: arrBuf.byteLength,
            });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: true,
              splitCount: numParts,
              rotated: false,
              finalW: parts[0].width,
              finalH: parts[0].height,
              finalSize: totalSize,
              imageState: 2,
            },
          });
        }
      }
      // ========================================================================
      // STATE 3: Rotate & Fit (Rotate 90°, then scale to fit 480x800, no split)
      // ========================================================================
      else if (imageState === 3) {
        // Step 1: Rotate 90° based on handedness
        const rotW = origH;
        const rotH = origW;

        const rotCanvas = document.createElement("canvas");
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext("2d");
        rotCtx.fillStyle = "#FFF";
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === "right";
        if (isClockwise) {
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(img, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0);

        // Step 2: Scale to fit 480x800 (if needed)
        const fitsInScreen = rotW <= MAX_WIDTH && rotH <= MAX_HEIGHT;

        if (fitsInScreen) {
          // Already fits after rotation - just apply grayscale
          applyGrayscale(rotCtx, rotW, rotH);
          const blob = await new Promise((res) => rotCanvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: "", width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: false,
              rotated: true,
              finalW: rotW,
              finalH: rotH,
              finalSize: arrBuf.byteLength,
              imageState: 3,
            },
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / rotW, MAX_HEIGHT / rotH);
          const newW = Math.round(rotW * scale);
          const newH = Math.round(rotH * scale);

          const scaledCanvas = document.createElement("canvas");
          scaledCanvas.width = newW;
          scaledCanvas.height = newH;
          const scaledCtx = scaledCanvas.getContext("2d");
          scaledCtx.imageSmoothingEnabled = true;
          scaledCtx.imageSmoothingQuality = "high";
          scaledCtx.fillStyle = "#FFF";
          scaledCtx.fillRect(0, 0, newW, newH);
          scaledCtx.drawImage(rotCanvas, 0, 0, newW, newH);
          applyGrayscale(scaledCtx, newW, newH);

          const blob = await new Promise((res) => scaledCanvas.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: "", width: newW, height: newH, size: arrBuf.byteLength }],
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: false,
              rotated: true,
              finalW: newW,
              finalH: newH,
              finalSize: arrBuf.byteLength,
              imageState: 3,
            },
          });
        }
      }
      // ========================================================================
      // STATE 0: Normal processing (scale to fit, no split/rotation)
      // ========================================================================
      else {
        // Normal processing: check if scaling is needed
        const fitsInScreen = origW <= MAX_WIDTH && origH <= MAX_HEIGHT;

        if (fitsInScreen) {
          // Image already fits - just convert to JPEG with grayscale
          const c = document.createElement("canvas");
          c.width = origW;
          c.height = origH;
          const ctx = c.getContext("2d");
          ctx.fillStyle = "#FFF";
          ctx.fillRect(0, 0, origW, origH);
          ctx.drawImage(img, 0, 0);
          applyGrayscale(ctx, origW, origH);

          const blob = await new Promise((res) => c.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: "", width: origW, height: origH, size: arrBuf.byteLength }],
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: false,
              rotated: false,
              finalW: origW,
              finalH: origH,
              finalSize: arrBuf.byteLength,
              imageState: 0,
            },
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / origW, MAX_HEIGHT / origH);
          const newW = Math.round(origW * scale);
          const newH = Math.round(origH * scale);

          const c = document.createElement("canvas");
          c.width = newW;
          c.height = newH;
          const ctx = c.getContext("2d");
          ctx.imageSmoothingEnabled = true;
          ctx.imageSmoothingQuality = "high";
          ctx.fillStyle = "#FFF";
          ctx.fillRect(0, 0, newW, newH);
          ctx.drawImage(img, 0, 0, newW, newH);
          applyGrayscale(ctx, newW, newH);

          const blob = await new Promise((res) => c.toBlob(res, "image/jpeg", JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: "", width: newW, height: newH, size: arrBuf.byteLength }],
            meta: {
              origW,
              origH,
              origSize,
              wasSplit: false,
              rotated: false,
              finalW: newW,
              finalH: newH,
              finalSize: arrBuf.byteLength,
              imageState: 0,
            },
          });
        }
      }
    };
    img.onerror = () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      reject(new Error("Image load failed"));
    };
    img.src = url;
  });
}

function imageMimeType(filename) {
  const lower = (filename || "").toLowerCase();
  if (lower.endsWith(".svg")) return "image/svg+xml";
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".gif")) return "image/gif";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".bmp")) return "image/bmp";
  return "image/jpeg";
}

const CROSSINK_OPTIMIZER_MANIFEST_PATH = "META-INF/crossink/optimizer-v1.json";
const CROSSINK_PXC_DIR = "META-INF/crossink/pxc";
const CROSSINK_BAYER_4X4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];

function crossInkPxcRules(text) {
  const rules = [];
  for (const match of text.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    const declarations = {};
    for (const declaration of match[2].matchAll(/(?:^|;)\s*([\w-]+)\s*:\s*([^;]+)/g)) {
      declarations[declaration[1].toLowerCase()] = declaration[2].trim().toLowerCase();
    }
    if (Object.keys(declarations).length === 0) continue;
    for (const selector of match[1].split(",")) rules.push({ selector: selector.trim(), declarations });
  }
  return rules;
}

function crossInkPxcStyle(rules, image) {
  const style = {};
  for (const rule of rules) {
    try {
      if (image.matches(rule.selector)) Object.assign(style, rule.declarations);
    } catch (_) {
      // Unsupported/invalid selectors must not style unrelated images.
    }
  }
  const inline = crossInkPxcRules(`x{${image.getAttribute("style") || ""}}`)[0];
  if (inline) Object.assign(style, inline.declarations);
  return style;
}

function crossInkPxcLength(value, relative) {
  if (!value) return undefined;
  const parsed = Number.parseFloat(value);
  if (!Number.isFinite(parsed)) return undefined;
  if (value.endsWith("%") || value.endsWith("vw")) return Math.max(1, Math.round(relative * parsed / 100));
  if (!value.endsWith("px") && !/^\d+(?:\.\d+)?$/.test(value)) return undefined;
  return Math.max(1, Math.round(parsed));
}

function crossInkPxcSize(width, height, style, viewportWidth, viewportHeight) {
  let resolvedWidth = crossInkPxcLength(style.width, viewportWidth);
  let resolvedHeight = crossInkPxcLength(style.height, viewportHeight);
  if (!resolvedWidth && !resolvedHeight) return { width, height };
  if (!resolvedWidth) resolvedWidth = Math.max(1, Math.round(resolvedHeight * width / height));
  if (!resolvedHeight) resolvedHeight = Math.max(1, Math.round(resolvedWidth * height / width));
  if (resolvedWidth > viewportWidth || resolvedHeight > viewportHeight) {
    const scale = Math.min(viewportWidth / resolvedWidth, viewportHeight / resolvedHeight);
    resolvedWidth = Math.max(1, Math.round(resolvedWidth * scale));
    resolvedHeight = Math.max(1, Math.round(resolvedHeight * scale));
  }
  return { width: resolvedWidth, height: resolvedHeight };
}

function crossInkPxcPathKey(value) {
  let hash = 0x811c9dc5;
  for (let index = 0; index < value.length; index++) hash = Math.imul(hash ^ value.charCodeAt(index), 0x01000193) >>> 0;
  return hash.toString(16).padStart(8, "0");
}

function crossInkCrc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function crossInkPackBits(data) {
  const out = [];
  let i = 0;
  while (i < data.length) {
    let run = 1;
    while (run < 128 && i + run < data.length && data[i + run] === data[i]) run++;
    if (run >= 3) { out.push(257 - run, data[i]); i += run; continue; }
    const start = i;
    while (i < data.length && i - start < 128) {
      if (i + 2 < data.length && data[i] === data[i + 1] && data[i] === data[i + 2]) break;
      i++;
    }
    out.push(i - start - 1, ...data.subarray(start, i));
  }
  return new Uint8Array(out);
}

function encodeCrossInkPxc2(legacy) {
  const input = new DataView(legacy.buffer, legacy.byteOffset, legacy.byteLength);
  const width = input.getUint16(0, true), height = input.getUint16(2, true);
  const raw = legacy.subarray(4), rowBytes = Math.ceil(width / 4);
  if (!width || !height || width > 1024 || height > 1024 || raw.length !== rowBytes * height || raw.length > 131072)
    throw new Error("Unsupported optimized image dimensions");
  const blocks = [];
  let size = 32;
  for (let offset = 0; offset < raw.length; offset += 2048) {
    const block = raw.subarray(offset, offset + 2048);
    let encoded = crossInkPackBits(block), codec = 2;
    if (encoded.length >= block.length) { encoded = block; codec = 0; }
    blocks.push({ block, encoded, codec }); size += 12 + encoded.length;
  }
  const out = new Uint8Array(size), view = new DataView(out.buffer);
  out.set([80, 88, 67, 50, 2, 1]); view.setUint16(6, 32, true);
  view.setUint16(8, width, true); view.setUint16(10, height, true); view.setUint16(12, rowBytes, true);
  view.setUint16(14, 2048, true); view.setUint16(16, blocks.length, true);
  view.setUint32(20, raw.length, true); view.setUint32(24, crossInkCrc32(raw), true); view.setUint32(28, size, true);
  let offset = 32;
  blocks.forEach(({ block, encoded, codec }, sequence) => {
    out[offset] = codec; view.setUint16(offset + 2, block.length, true);
    view.setUint16(offset + 4, encoded.length, true); view.setUint16(offset + 6, sequence, true);
    view.setUint32(offset + 8, crossInkCrc32(block), true); out.set(encoded, offset + 12); offset += 12 + encoded.length;
  });
  return out;
}

function crossInkIndexPath(path, limit) {
  return path && !path.startsWith("/") && !path.includes("..") && !/[\x00-\x1f\\:%]/.test(path) &&
    new TextEncoder().encode(path).length <= limit;
}

function buildCrossInkImageIndex(manifest, entries) {
  if (entries.length > 256) throw new Error("Too many optimizer index records");
  const out = new Uint8Array(32 + 208 * entries.length), view = new DataView(out.buffer), encoder = new TextEncoder();
  const seen = new Set();
  entries.forEach((e, i) => {
    if (seen.has(e.href) || !crossInkIndexPath(e.href, 128) || !crossInkIndexPath(e.pxc, 64))
      throw new Error("Invalid optimizer index path");
    seen.add(e.href);
    const at = 32 + 208 * i;
    out.set(encoder.encode(e.href), at); out.set(encoder.encode(e.pxc), at + 129);
    view.setUint16(at + 194, e.width, true); view.setUint16(at + 196, e.height, true); out[at + 198] = 2;
    view.setUint32(at + 200, e.pxcBytes, true); view.setUint32(at + 204, e.pixelCrc32, true);
  });
  out.set([67, 79, 73, 88]); view.setUint16(4, 1, true); view.setUint16(6, 32, true);
  view.setUint16(8, 208, true); view.setUint16(10, entries.length, true);
  view.setUint32(16, crossInkCrc32(manifest), true); view.setUint32(20, manifest.length, true);
  view.setUint32(24, crossInkCrc32(out.subarray(32)), true); view.setUint32(28, crossInkCrc32(out.subarray(0, 28)), true);
  return out;
}

async function buildCrossInkPxc(data, width, height) {
  const bitmap = await createImageBitmap(new Blob([data]));
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.fillStyle = "#fff";
  ctx.fillRect(0, 0, width, height);
  ctx.drawImage(bitmap, 0, 0, width, height);
  if (bitmap.close) bitmap.close();
  const pixels = ctx.getImageData(0, 0, width, height).data;
  const rowBytes = Math.ceil(width / 4);
  const output = new Uint8Array(4 + rowBytes * height);
  const view = new DataView(output.buffer);
  view.setUint16(0, width, true);
  view.setUint16(2, height, true);
  let offset = 4;
  for (let y = 0; y < height; y++) {
    let packed = 0;
    let shift = 6;
    for (let x = 0; x < width; x++) {
      const index = (y * width + x) * 4;
      const grayBase = Math.round(pixels[index] * 0.299 + pixels[index + 1] * 0.587 + pixels[index + 2] * 0.114);
      const gray = Math.max(0, Math.min(255, grayBase + (CROSSINK_BAYER_4X4[y & 3][x & 3] - 8) * 5));
      const level = gray < 64 ? 0 : gray < 128 ? 1 : gray < 192 ? 2 : 3;
      packed |= level << shift;
      if (shift === 0) {
        output[offset++] = packed;
        packed = 0;
        shift = 6;
      } else {
        shift -= 2;
      }
    }
    if (shift !== 6) output[offset++] = packed;
  }
  return encodeCrossInkPxc2(output);
}

function resolveCrossInkPxcPath(basePath, reference) {
  const source = decodeHref((reference || "").split("#")[0]);
  if (!source || source.startsWith("data:")) return "";
  return resolvePath(basePath, source);
}

async function buildCrossInkPxcSidecars(out, zip, xhtmlFiles) {
  const cssRules = new Map();
  for (const [path, fileObj] of Object.entries(zip.files)) {
    if (!fileObj.dir && path.toLowerCase().endsWith(".css")) cssRules.set(path, crossInkPxcRules(await safeReadText(fileObj)));
  }
  const entries = [];
  const seen = new Set();
  // The display is landscape while the UI's conversion bounds are portrait.
  const viewportWidth = MAX_HEIGHT;
  const viewportHeight = MAX_WIDTH;
  for (const [xhtmlPath, xhtml] of Object.entries(xhtmlFiles)) {
    const doc = new DOMParser().parseFromString(xhtml, "text/html");
    const rules = [];
    doc.querySelectorAll('link[rel="stylesheet"]').forEach((link) => {
      rules.push(...(cssRules.get(resolveCrossInkPxcPath(xhtmlPath, link.getAttribute("href"))) || []));
    });
    doc.querySelectorAll("style").forEach((style) => rules.push(...crossInkPxcRules(style.textContent || "")));
    for (const image of doc.querySelectorAll("img")) {
      const href = resolveCrossInkPxcPath(xhtmlPath, image.getAttribute("src"));
      const imageFile = href && out.file(href);
      if (!imageFile) continue;
      try {
        const data = await imageFile.async("arraybuffer");
        const bitmap = await createImageBitmap(new Blob([data]));
        const sourceWidth = bitmap.width;
        const sourceHeight = bitmap.height;
        if (bitmap.close) bitmap.close();
        if (!sourceWidth || !sourceHeight) continue;
        const size = crossInkPxcSize(sourceWidth, sourceHeight, crossInkPxcStyle(rules, image), viewportWidth, viewportHeight);
        if (entries.length >= 256 || entries.some((e) => e.href === href) || !crossInkIndexPath(href, 128)) continue;
        const key = `${href}:${size.width}x${size.height}`;
        if (seen.has(key)) continue;
        const pxcPath = `${CROSSINK_PXC_DIR}/${crossInkPxcPathKey(key)}.pxc2`;
        const payload = await buildCrossInkPxc(data, size.width, size.height);
        out.file(pxcPath, payload, { compression: "STORE", createFolders: false });
        seen.add(key);
        entries.push({ href, pxc: pxcPath, ...size, pxcFormat: "pxc2", pxcBytes: payload.length,
          pixelCrc32: new DataView(payload.buffer).getUint32(24, true) });
      } catch (_) {
        // Sidecars are optional; keep the source EPUB image when a browser cannot decode it.
      }
    }
  }
  return entries;
}

// Convert EPUB file - returns converted blob
async function convertEpubFile(file, progressCallback) {
  const startTime = Date.now();
  const originalSize = file.size;

  // Initialize logging
  clearLog();
  showLog();
  log(`<strong>${file.name}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, "", "INFO");
  log(
    `Quality: ${JPEG_QUALITY}% | Overlap: ${OVERLAP_PERCENT}% | Rotation: ${HANDEDNESS === "right" ? "CW" : "CCW"} | Grayscale: ${ENABLE_GRAYSCALE ? "ON" : "OFF"}`,
    "",
    "INFO",
  );

  const zip = await JSZip.loadAsync(file);
  await assertEpubHasNoContentEncryption(zip);
  const renamed = {};
  zip.forEach((p) => {
    const l = p.toLowerCase();
    if (l.match(/\.(png|gif|webp|bmp|jpeg|svg)$/)) {
      renamed[p] = p.replace(/\.(png|gif|webp|bmp|jpeg|svg)$/i, ".jpg");
    }
  });

  const out = new JSZip();
  const entries = Object.entries(zip.files);
  const coverImagePaths = document.getElementById("preserveCoverColorToggle")?.checked
    ? await findEpubCoverImagePaths(zip)
    : new Set();
  const splitImages = {};
  const xhtmlFiles = {};
  let processedXhtmlFiles = {};
  let extraTextFiles = {};
  let opfPath = null,
    opfContent = null;
  let mainIdentifier = null;
  let optimizedOpfContent = null;

  // Write mimetype FIRST per EPUB OCF spec
  if (zip.files["mimetype"]) {
    const mimetypeData = await zip.files["mimetype"].async("arraybuffer");
    out.file("mimetype", mimetypeData, { compression: "STORE", createFolders: false });
  }

  // First pass: process images
  for (let i = 0; i < entries.length; i++) {
    if (operationCancelled) throw new Error("Cancelled by user");
    const [path, fileObj] = entries[i];
    if (fileObj.dir || path === "mimetype") continue;
    const low = path.toLowerCase();

    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg|svg)$/)) {
      const data = await fileObj.async("arraybuffer");
      const imageState = getImageState(path);

      let result;
      try {
        result = await processImage(data, imageState, path, coverImagePaths.has(path));
      } catch (imageError) {
        // Log error but continue with original image
        console.error(`Failed to process image ${path}:`, imageError);
        log(`Warning: Failed to process ${path.split("/").pop()}, using original`, "warning", "IMG-ERR");
        delete renamed[path];
        out.file(path, data, { compression: "STORE", createFolders: false });
        if (progressCallback) progressCallback((i / entries.length) * 60);
        continue;
      }

      const parts = result.parts;
      const meta = result.meta;

      const baseName = path.replace(/\.[^.]+$/, "");
      const newExt = ".jpg";

      // Log image processing
      const imgName = path.split("/").pop();
      const origFormat = path.split(".").pop();
      logImage(
        imgName,
        meta.origW,
        meta.origH,
        origFormat,
        meta.origSize,
        meta.finalW,
        meta.finalH,
        meta.finalSize,
        meta.wasSplit,
        meta.splitCount || 0,
        parts,
        meta.imageState || 0,
      );

      if (parts.length === 1 && parts[0].suffix === "") {
        const newPath = renamed[path] || path.replace(/\.[^.]+$/, newExt);
        out.file(newPath, parts[0].data, { compression: "STORE", createFolders: false });
      } else {
        // Store with full path for collision prevention, but also keep original filename
        const origName = path.split("/").pop();
        const origDir = path.includes("/") ? path.substring(0, path.lastIndexOf("/")) : "";

        // Key by full path to avoid collisions
        splitImages[path] = {
          origName: origName,
          origDir: origDir,
          parts: [],
        };

        for (const part of parts) {
          const partName = baseName.split("/").pop() + part.suffix + newExt;
          const partPath = (path.includes("/") ? path.substring(0, path.lastIndexOf("/") + 1) : "") + partName;
          out.file(partPath, part.data, { compression: "STORE", createFolders: false });
          // Store metadata for XHTML/OPF updates
          splitImages[path].parts.push({
            path: partPath,
            imgName: partName,
            id: baseName.split("/").pop() + part.suffix,
            suffix: part.suffix,
          });
        }
      }
    } else if (low.match(/\.(xhtml|html|htm)$/)) {
      xhtmlFiles[path] = await safeReadText(fileObj);
    } else if (low.endsWith(".opf")) {
      opfPath = path;
      opfContent = await safeReadText(fileObj);
    }

    if (progressCallback) progressCallback((i / entries.length) * 60);
  }

  // Second pass: update XHTML using DOMParser
  for (const [xhtmlPath, content] of Object.entries(xhtmlFiles)) {
    if (operationCancelled) throw new Error("Cancelled by user");
    let t = scrubEpubTextResource(xhtmlPath, content);

    const stripped = stripComments(t);
    if (stripped.count) {
      t = stripped.text;
      logFix(`Comments (${stripped.count}, ${formatBytes(stripped.bytes)})`, xhtmlPath.split("/").pop());
    }

    const r = fixSvgCover(t);
    if (r.fixed) {
      t = r.c;
      logFix("SVG cover", xhtmlPath.split("/").pop());
    }

    const r2 = fixSvgWrappedImages(t);
    if (r2.fixed) {
      t = r2.c;
      logFix(`SVG images (${r2.count})`, xhtmlPath.split("/").pop());
    }

    // Use DOMParser for all img modifications: remove width/height and handle split images
    try {
      const whitespaceGuard = protectWhitespaceOnlyTextNodes(t);
      const parser = new DOMParser();
      const doc = parser.parseFromString(whitespaceGuard.content, "application/xhtml+xml");
      const parseError = doc.querySelector("parsererror");

      if (!parseError) {
        let modified = false;

        // Remove width/height attributes from ALL img tags (dimensions may have changed)
        // This prevents CrossInk and other readers from using wrong dimensions
        const allImgElements = doc.querySelectorAll("img");
        for (const img of allImgElements) {
          if (img.hasAttribute("width")) {
            img.removeAttribute("width");
            modified = true;
          }
          if (img.hasAttribute("height")) {
            img.removeAttribute("height");
            modified = true;
          }

          const src = img.getAttribute("src");
          if (src) {
            const renamedSrc = renamedImageSrc(src, xhtmlPath, renamed);
            if (renamedSrc.changed) {
              img.setAttribute("src", renamedSrc.src);
              modified = true;
            }
          }
        }

        // Handle split images with path collision prevention
        if (Object.keys(splitImages).length > 0) {
          // Get XHTML directory for resolving relative paths
          const xhtmlDir = xhtmlPath.includes("/") ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf("/")) : "";
          const rootFolders = ["ops", "oebps", "epub", "content"];

          for (const [fullPath, splitInfo] of Object.entries(splitImages)) {
            const origName = splitInfo.origName;
            const origDir = splitInfo.origDir;
            const parts = splitInfo.parts;
            const newName = origName.replace(/\.(png|gif|webp|bmp|jpeg|svg)$/i, ".jpg");

            // Extract immediate parent directory for collision prevention
            const splitDirParts = origDir.split("/").filter((p) => p);
            const lastDir = splitDirParts.length > 0 ? splitDirParts[splitDirParts.length - 1].toLowerCase() : null;
            const immediateParent =
              lastDir && !rootFolders.includes(lastDir) ? splitDirParts[splitDirParts.length - 1] : null;

            // Get XHTML's parent directory parts for relative path resolution
            const xhtmlDirParts = xhtmlDir.split("/").filter((p) => p);

            // Find all img elements
            const allImgs = doc.querySelectorAll("img");
            const matchingImgs = [];

            for (const img of allImgs) {
              const src = img.getAttribute("src") || "";
              const srcParts = src.split("/").filter((p) => p && p !== ".." && p !== ".");
              const srcName = srcParts.pop() || "";

              // Check filename match
              if (srcName !== origName && srcName !== newName) continue;

              // Path collision prevention with root folder handling
              if (immediateParent) {
                // Image is in a specific subfolder (not root like OPS/OEBPS)
                if (srcParts.length === 0) {
                  // src has no path - check if XHTML is in same folder as image
                  const xhtmlLastDir = xhtmlDirParts.length > 0 ? xhtmlDirParts[xhtmlDirParts.length - 1] : null;
                  if (xhtmlLastDir !== immediateParent) continue;
                } else {
                  // src has path - verify parent directory matches
                  if (srcParts[srcParts.length - 1] !== immediateParent) continue;
                }
              } else {
                // Image is in root folder (like OEBPS/cover.jpg)
                // Only match if src has NO subfolder path OR points to root folder
                if (srcParts.length > 0) {
                  const srcLastDir = srcParts[srcParts.length - 1].toLowerCase();
                  if (!rootFolders.includes(srcLastDir)) continue;
                }
              }

              matchingImgs.push(img);
            }

            // Process each matching img — Pro's strip+inject approach
            for (const img of matchingImgs) {
              const src = img.getAttribute("src") || "";

              // Part 1: update src in-place, strip original sizing
              img.setAttribute("src", src.replace(origName, parts[0].imgName).replace(newName, parts[0].imgName));

              if (parts.length > 1) {
                // Strip original width/height/class that were sized for the unsplit image
                img.removeAttribute("width");
                img.removeAttribute("height");
                img.removeAttribute("class");
                img.setAttribute("style", "max-width:100%;height:auto");

                // Neutralize container height constraints that were sized for the original
                let container = img.parentElement;
                const safeContainers = ["div", "p", "figure", "aside", "section", "body"];
                while (container && !safeContainers.includes(container.tagName.toLowerCase()))
                  container = container.parentElement;
                const insertTarget = container || img.parentElement;
                // Strip constraining classes/styles from container — they were for the unsplit image
                if (insertTarget && insertTarget.tagName.toLowerCase() !== "body") {
                  insertTarget.removeAttribute("class");
                  insertTarget.removeAttribute("style");
                }
                const insertParent = insertTarget.parentElement;
                const insertRef = insertTarget.nextSibling;
                const ns = doc.documentElement.namespaceURI || "http://www.w3.org/1999/xhtml";

                // Insert new minimal wrappers for parts 2+ in reading order
                for (let pi = 1; pi < parts.length; pi++) {
                  const wrapper = doc.createElementNS(ns, "div");
                  const newImg = doc.createElementNS(ns, "img");
                  const partSrc = src.replace(origName, parts[pi].imgName).replace(newName, parts[pi].imgName);
                  newImg.setAttribute("src", partSrc);
                  newImg.setAttribute("alt", "");
                  newImg.setAttribute("style", "max-width:100%;height:auto");
                  wrapper.appendChild(newImg);
                  if (insertRef) insertParent.insertBefore(wrapper, insertRef);
                  else insertParent.appendChild(wrapper);
                }
              }
              modified = true;
            }
          }
        }

        // Only serialize if we made changes
        if (modified) {
          t = whitespaceGuard.restore(safeSerialize(doc, whitespaceGuard.content));
        }
      } else {
        const fallback = rewriteImageSrcReferences(t, xhtmlPath, renamed, splitImages);
        if (fallback.changed) t = fallback.content;
      }
    } catch (e) {
      console.warn("DOMParser error for", xhtmlPath, e.message);
      const fallback = rewriteImageSrcReferences(t, xhtmlPath, renamed, splitImages);
      if (fallback.changed) t = fallback.content;
    }

    // Inject universal image constraint — prevents overflow on e-ink displays
    if (t.includes("</head>")) {
      t = t.replace("</head>", DEFENSIVE_STYLE + "</head>");
    }

    processedXhtmlFiles[xhtmlPath] = t;
  }

  const collapseResult = collapseReaderEmptySpineItems(processedXhtmlFiles, opfContent, opfPath);
  opfContent = collapseResult.opfContent;
  for (const [xhtmlPath, content] of Object.entries(processedXhtmlFiles)) {
    processedXhtmlFiles[xhtmlPath] = rewriteCollapsedSpineReferences(content, xhtmlPath, collapseResult.redirects);
  }
  if (collapseResult.redirects.size > 0) {
    logFix("EPUB sections", `Collapsed ${collapseResult.redirects.size} empty chapter stubs`);
  }

  const splitLongSections = !!document.getElementById("splitLongSectionsToggle")?.checked;
  const sectionSplitResult = splitLongXhtmlSections(processedXhtmlFiles, opfContent, opfPath, splitLongSections);
  processedXhtmlFiles = sectionSplitResult.files;
  for (const [xhtmlPath, content] of Object.entries(processedXhtmlFiles)) {
    processedXhtmlFiles[xhtmlPath] = rewriteSplitSectionReferences(
      content,
      xhtmlPath,
      sectionSplitResult.anchorTargets,
    );
  }
  if (Object.keys(sectionSplitResult.splitSections).length > 0) {
    const splitPartCount = Object.values(sectionSplitResult.splitSections).reduce(
      (sum, parts) => sum + parts.length,
      0,
    );
    logFix(
      "EPUB sections",
      `${splitPartCount} parts from ${Object.keys(sectionSplitResult.splitSections).length} long sections`,
    );
  }

  // Extract main identifier from OPF using DOMParser with regex fallback
  if (opfContent) {
    mainIdentifier = extractIdentifier(opfContent);
  }

  for (const [path, fileObj] of entries) {
    if (fileObj.dir || path === "mimetype") continue;
    const low = path.toLowerCase();
    if (low.endsWith(".ncx") || low.match(/\.(xml|svg)$/)) {
      const content = rewriteCollapsedSpineReferences(
        scrubEpubTextResource(path, await safeReadText(fileObj)),
        path,
        collapseResult.redirects,
      );
      extraTextFiles[path] = rewriteSplitSectionReferences(content, path, sectionSplitResult.anchorTargets);
    }
  }

  // Third pass: update OPF using fixOPF (DOMParser with regex fallback)
  if (opfContent) {
    let t = scrubEpubTextResource(opfPath, opfContent);
    for (const [o, n] of Object.entries(renamed)) {
      t = t.split(o.split("/").pop()).join(n.split("/").pop());
    }
    const opfDir = opfPath.includes("/") ? opfPath.substring(0, opfPath.lastIndexOf("/")) : "";
    t = fixOPF(t, opfContent, opfDir, splitImages);
    t = addSplitSectionsToOpf(t, opfPath, sectionSplitResult.splitSections);
    t = rewriteSplitSectionReferences(t, opfPath, sectionSplitResult.anchorTargets);
    optimizedOpfContent = t;
    if (t !== opfContent) logFix("OPF", "manifest updated");
    out.file(opfPath, t, DEFLATE_OPTS);

    const referenceCharactersInput = document.getElementById("referenceCharactersInput");
    const referenceCharactersPerPage = normalizedReferenceCharactersPerPage(referenceCharactersInput?.value);
    if (referenceCharactersInput) referenceCharactersInput.value = referenceCharactersPerPage;
    const locationManifest = buildXLocationManifest(
      t,
      opfPath,
      processedXhtmlFiles,
      referenceCharactersPerPage,
      sectionSplitResult.sourceSpineMap,
    );
    if (locationManifest) {
      out.file(X_LOCATION_MANIFEST_PATH, JSON.stringify(locationManifest), DEFLATE_OPTS);
      logFix(
        "X locations",
        `${locationManifest.totalLocations} locations, ${locationManifest.totalReferencePages} reference pages`,
      );
    }
  }

  for (const [xhtmlPath, content] of Object.entries(processedXhtmlFiles)) {
    out.file(xhtmlPath, content, DEFLATE_OPTS);
  }

  const imageCacheEntries = await buildCrossInkPxcSidecars(out, zip, processedXhtmlFiles);
  if (optimizedOpfContent) {
    const manifest = new TextEncoder().encode(JSON.stringify({
      format: "crossink-optimizer",
      version: 1,
      target: { device: ACTIVE_DEVICE.toLowerCase(), width: MAX_HEIGHT, height: MAX_WIDTH, grayscaleLevels: 4 },
      generator: "crossink-web-uploader",
      features: { htmlNormalized: true, cssFlattened: false, xLocations: true, prebuiltPxc: imageCacheEntries.length > 0 },
      images: imageCacheEntries,
    }));
    out.file(CROSSINK_OPTIMIZER_MANIFEST_PATH, manifest, { compression: "STORE", createFolders: false });
    out.file("META-INF/crossink/optimizer-images-v1.idx", buildCrossInkImageIndex(manifest, imageCacheEntries),
      { compression: "STORE", createFolders: false });
  }

  // Copy remaining files
  for (const [path, fileObj] of entries) {
    if (operationCancelled) throw new Error("Cancelled by user");
    if (fileObj.dir || path === "mimetype") continue;
    const low = path.toLowerCase();
    if (low === X_LOCATION_MANIFEST_PATH.toLowerCase() || low === CROSSINK_OPTIMIZER_MANIFEST_PATH.toLowerCase() || low === "meta-inf/crossink/optimizer-images-v1.idx" ||
        low.startsWith(`${CROSSINK_PXC_DIR.toLowerCase()}/`)) continue;
    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg|svg)$/) || low.match(/\.(xhtml|html|htm)$/) || low.endsWith(".opf"))
      continue;

    let data = await fileObj.async("arraybuffer");
    if (low.endsWith(".css")) {
      let t = scrubEpubTextResource(path, await safeReadText(fileObj));
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split("/").pop()).join(n.split("/").pop());
      }
      data = new TextEncoder().encode(t);
    } else if (low.endsWith(".ncx")) {
      let t = extraTextFiles[path] || scrubEpubTextResource(path, await safeReadText(fileObj));
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split("/").pop()).join(n.split("/").pop());
      }
      const oldT = t;
      t = syncNCXIdentifier(t, mainIdentifier);
      if (t !== oldT) logFix("NCX identifier", "Synced with OPF");
      data = new TextEncoder().encode(t);
    } else if (low.match(/\.(xml|svg)$/)) {
      const t = extraTextFiles[path] || scrubEpubTextResource(path, await safeReadText(fileObj));
      data = new TextEncoder().encode(t);
    }
    out.file(path, data, DEFLATE_OPTS);
  }

  if (progressCallback) progressCallback(100);

  // Generate final blob
  const newBlob = await out.generateAsync({ type: "blob", mimeType: "application/epub+zip" });
  const newSize = newBlob.size;
  const timeElapsed = (Date.now() - startTime) / 1000;

  // Log completion
  log("Conversion complete!", "success", "DONE");
  logSummary(originalSize, newSize, timeElapsed);

  // Auto-export only if NOT in batch mode (batch mode exports at the end)
  if (!isBatchMode && exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, false); // isBatch = false for single file
    }, 100);
  }

  return newBlob;
}

async function assertEpubHasNoContentEncryption(zip) {
  const encryptionEntry = Object.entries(zip.files).find(
    ([path, fileObj]) => !fileObj.dir && path.toLowerCase() === "meta-inf/encryption.xml",
  );
  if (!encryptionEntry) return;

  const [, encryptionFile] = encryptionEntry;
  const encryptionXml = await safeReadText(encryptionFile);
  const document = new DOMParser().parseFromString(encryptionXml, "application/xml");
  if (document.querySelector("parsererror")) {
    throw new Error("This EPUB has invalid encryption metadata and cannot be optimized safely.");
  }

  const fontObfuscationAlgorithms = new Set([
    "http://www.idpf.org/2008/embedding",
    "http://ns.adobe.com/pdf/enc#RC",
  ]);
  for (const encryptedData of document.getElementsByTagNameNS("*", "EncryptedData")) {
    const method = encryptedData.getElementsByTagNameNS("*", "EncryptionMethod")[0];
    const algorithm = method?.getAttribute("Algorithm");

    // Publishers often store obfuscated fonts as .dat files, so the algorithm
    // is the reliable signal; their filename is not.
    if (!fontObfuscationAlgorithms.has(algorithm)) {
      throw new Error("This EPUB is DRM-protected. Please remove DRM before optimizing it.");
    }
  }
}

// Get WebSocket URL based on current page location
function getWsUrl() {
  const host = window.location.hostname;
  return `ws://${host}:${WS_PORT}/`;
}

// Upload file via WebSocket (faster, binary protocol)
function uploadFileWebSocket(file, onProgress, onComplete, onError) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(getWsUrl());
    currentUploadWs = ws;
    let uploadStarted = false;
    let sendingChunks = false;
    let uploadComplete = false; // set only when DONE is received and resolve() called

    ws.binaryType = "arraybuffer";

    ws.onopen = function () {
      console.log("[WS] Connected, starting upload:", file.name);
      // Send start message: START:<filename>:<size>:<path>
      ws.send(`START:${file.name}:${file.size}:${currentPath}`);
    };

    ws.onmessage = async function (event) {
      const msg = event.data;
      console.log("[WS] Message:", msg);

      if (msg === "READY") {
        uploadStarted = true;
        sendingChunks = true;

        // Small delay to let connection stabilize
        await new Promise((r) => setTimeout(r, 50));

        try {
          // Send file in chunks
          const totalSize = file.size;
          let offset = 0;

          while (offset < totalSize && ws.readyState === WebSocket.OPEN) {
            const chunkSize = Math.min(WS_CHUNK_SIZE, totalSize - offset);
            const chunk = file.slice(offset, offset + chunkSize);
            const buffer = await chunk.arrayBuffer();

            // Wait for buffer to clear - more aggressive backpressure
            while (ws.bufferedAmount > WS_CHUNK_SIZE * 2 && ws.readyState === WebSocket.OPEN) {
              await new Promise((r) => setTimeout(r, 5));
            }

            if (ws.readyState !== WebSocket.OPEN) {
              throw new Error("WebSocket closed during upload");
            }

            ws.send(buffer);
            offset += chunkSize;

            // Update local progress with real transfer progress
            // Server will confirm 100% with DONE message
            if (onProgress) {
              onProgress(offset, totalSize);
            }
          }

          sendingChunks = false;
          console.log("[WS] All chunks sent, waiting for DONE");
        } catch (err) {
          console.error("[WS] Error sending chunks:", err);
          sendingChunks = false;
          ws.close();
          reject(err);
        }
      } else if (msg.startsWith("PROGRESS:")) {
        // Server confirmed progress - log for debugging but don't update UI
        // (local progress is smoother, server progress causes jumping)
        console.log("[WS] Server progress:", msg);
      } else if (msg === "DONE") {
        // Show 100% when server confirms completion
        if (onProgress) onProgress(file.size, file.size);
        uploadComplete = true;
        currentUploadWs = null;
        ws.close();
        if (onComplete) onComplete();
        resolve();
      } else if (msg.startsWith("ERROR:")) {
        const error = msg.substring(6);
        ws.close();
        if (onError) onError(error);
        reject(new Error(error));
      }
    };

    ws.onerror = function (event) {
      console.error("[WS] Error:", event);
      currentUploadWs = null;
      if (!uploadStarted) {
        reject(new Error("WebSocket connection failed"));
      } else if (!sendingChunks) {
        reject(new Error("WebSocket error during upload"));
      } else {
        // Error during chunk sending - reject with appropriate message
        reject(new Error("WebSocket error during file transfer"));
      }
    };

    ws.onclose = function (event) {
      console.log("[WS] Connection closed, code:", event.code, "reason:", event.reason);
      // Reject for any close before upload was confirmed complete (covers both
      // mid-chunk-send closes and the "all chunks sent, waiting for DONE" window)
      if (!uploadComplete) {
        reject(new Error("WebSocket closed during upload"));
      }
    };
  });
}

// Upload file via HTTP (fallback method)
function uploadFileHTTP(file, onProgress, onComplete, onError) {
  return new Promise((resolve, reject) => {
    const formData = new FormData();
    formData.append("file", file);

    const xhr = new XMLHttpRequest();
    currentUploadXhr = xhr;
    xhr.open("POST", "/upload?path=" + encodeURIComponent(currentPath), true);

    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable && onProgress) {
        onProgress(e.loaded, e.total);
      }
    };

    xhr.onload = function () {
      currentUploadXhr = null;
      if (xhr.status === 200) {
        if (onComplete) onComplete();
        resolve();
      } else {
        const error = xhr.responseText || "Upload failed";
        if (onError) onError(error);
        reject(new Error(error));
      }
    };

    xhr.onerror = function () {
      currentUploadXhr = null;
      const error = "Network error";
      if (onError) onError(error);
      reject(new Error(error));
    };

    xhr.onabort = function () {
      currentUploadXhr = null;
      reject(new Error("Upload aborted"));
    };

    xhr.send(formData);
  });
}

async function uploadFile() {
  if (isUploadInProgress) return;

  const fileInput = document.getElementById("fileInput");
  const files = Array.from(fileInput.files);
  const convertEnabled = document.getElementById("convertBeforeUpload").checked;

  if (files.length === 0) {
    alert("Please select at least one file!");
    return;
  }

  isUploadInProgress = true;
  let usedFileNames;
  try {
    usedFileNames = await fetchExistingUploadNames();
  } catch (error) {
    isUploadInProgress = false;
    alert(`Failed to check existing files: ${error.message}`);
    return;
  }

  // Prevent modal close during upload
  uploadGeneration++;
  const myGeneration = uploadGeneration;
  document.getElementById("uploadModalClose").classList.add("disabled");
  fileInput.disabled = true;

  const progressContainer = document.getElementById("progress-container");
  const progressFill = document.getElementById("progress-fill");
  const progressText = document.getElementById("progress-text");
  const uploadBtn = document.getElementById("uploadBtn");

  progressContainer.style.display = "block";
  uploadBtn.disabled = true;

  let currentIndex = 0;
  const failedFiles = [];
  let useWebSocket = true; // Try WebSocket first

  // Check if we should use batch logging mode
  const epubFilesToConvert = files.filter((f) => f.name.toLowerCase().endsWith(".epub") && convertEnabled);
  const useBatchLog = epubFilesToConvert.length > 1 && exportLogCheckbox && exportLogCheckbox.checked;

  // Start batch log mode if needed
  if (useBatchLog) {
    startBatchLog(epubFilesToConvert.length);
    showLog();
  }

  async function uploadNextFile() {
    if (currentIndex >= files.length) {
      // All files processed - show summary
      if (failedFiles.length === 0) {
        progressFill.style.backgroundColor = "#4caf50";
        progressText.textContent = "All uploads complete!";

        // Finalize batch log if in batch mode
        if (useBatchLog) {
          finalizeBatchLog();
          setTimeout(() => {
            window.location.reload();
          }, 2000);
        } else {
          setTimeout(() => {
            window.location.reload();
          }, 1000);
        }
      } else {
        progressFill.style.backgroundColor = "#e74c3c";
        const failedList = failedFiles.map((f) => f.name).join(", ");
        progressText.textContent = `${files.length - failedFiles.length}/${files.length} uploaded. Failed: ${failedList}`;

        // Add upload errors to batch log
        if (useBatchLog) {
          failedFiles.forEach((ff) => {
            logError(`Upload failed for ${ff.name}: ${ff.error}`);
          });
          finalizeBatchLog();
        }

        // Only show banner if THIS upload session had failures
        // Use local failedFiles, not the global shared variable
        if (failedFiles.length > 0) {
          // Accumulate failed uploads to global (don't replace)
          failedUploadsGlobal = failedUploadsGlobal.concat(failedFiles);
          // Clear flag and close modal, then show banner with retry options
          isUploadInProgress = false;
          document.getElementById("uploadModalClose").classList.remove("disabled");
          closeUploadModal();
          showFailedUploadsBanner();
        }
      }
      return;
    }

    let file = files[currentIndex];
    const originalFile = file;
    // Reset progress bar instantly without transition when starting a new file
    progressFill.classList.add("no-transition");
    progressFill.style.width = "0%";
    progressFill.style.backgroundColor = "#27ae60";
    // Re-enable transition after a brief delay
    setTimeout(() => progressFill.classList.remove("no-transition"), 50);

    // Check if file is an EPUB and conversion is enabled
    const isEpub = file.name.toLowerCase().endsWith(".epub");
    const needsConversion = isEpub && convertEnabled;
    let conversionSucceeded = false;
    let conversionFailed = false; // Track if conversion actually failed
    let convOriginalSize = 0; // Picked-file size; 0 unless conversion succeeded
    let convNewSize = 0; // Generated blob size; 0 unless conversion succeeded

    if (isEpub && document.getElementById("renameFromMetadataToggle").checked) {
      const originalName = file.name;
      progressText.style.color = "";
      progressText.textContent = `Reading metadata for ${file.name} (${currentIndex + 1}/${files.length})...`;
      file = await maybeRenameEbookFile(file);
      if (file.name !== originalName) console.log(`[Upload] Renamed from metadata: ${originalName} -> ${file.name}`);
    }

    const availableName = reserveAvailableUploadFilename(file.name, usedFileNames);
    if (availableName !== file.name) {
      console.log(`[Upload] Renamed to avoid collision: ${file.name} -> ${availableName}`);
      file = new File([file], availableName, {
        type: file.type,
        lastModified: file.lastModified,
      });
    }

    const methodText = useWebSocket ? " [WS]" : " [HTTP]";
    const stageText = needsConversion ? "Converting & uploading" : "Uploading";
    progressText.style.color = "";
    progressText.textContent = `${stageText} ${file.name} (${currentIndex + 1}/${files.length})${methodText}`;

    const onProgress = (loaded, total) => {
      const uploadPercent = Math.round((loaded / total) * 100);
      // If conversion succeeded, display goes from 50-100%, otherwise 0-100%
      const displayPercent = conversionSucceeded ? 50 + Math.round(uploadPercent / 2) : uploadPercent;
      progressFill.style.width = displayPercent + "%";
      const prefix = conversionSucceeded ? "Converting & uploading" : "Uploading";
      progressText.textContent = `${prefix} ${file.name} (${currentIndex + 1}/${files.length})${methodText} — ${uploadPercent}%`;
    };

    const onComplete = () => {
      // Save file log to batch if in batch mode and this file was converted
      // Consider it successful only if conversion didn't fail
      if (useBatchLog && needsConversion) {
        const ok = !conversionFailed && conversionSucceeded;
        saveToFileBatchLog(file.name, ok, ok ? convOriginalSize : 0, ok ? convNewSize : 0);
      }

      currentIndex++;
      uploadNextFile();
    };

    const onError = (error) => {
      // Save failed file log to batch if in batch mode
      if (useBatchLog && needsConversion) {
        logError(`Upload failed: ${error}`);
        // Preserve conversion size totals when the convert step succeeded but
        // the upload failed; otherwise nothing was produced and 0/0 is correct.
        saveToFileBatchLog(file.name, false, convOriginalSize, convNewSize);
      }

      failedFiles.push({ name: file.name, error: error, file: originalFile });

      // If network error, mark all remaining files as failed and show retry banner
      if (
        error.includes("connection failed") ||
        error.includes("Network error") ||
        error.includes("timeout") ||
        error.includes("disconnected")
      ) {
        console.log(`[Network] Network error detected: ${error}`);

        // Add all remaining files to failed list
        const remainingFiles = files.slice(currentIndex + 1);
        remainingFiles.forEach((remainingFile) => {
          failedFiles.push({
            name: remainingFile.name,
            error: "Network error - upload interrupted",
            file: remainingFile,
          });
        });

        // Show retry banner immediately with all failed files
        failedUploadsGlobal = failedUploadsGlobal.concat(failedFiles);
        isUploadInProgress = false;
        document.getElementById("uploadModalClose").classList.remove("disabled");
        closeUploadModal();
        showFailedUploadsBanner();
        return; // Stop processing
      }

      currentIndex++;
      uploadNextFile();
    };

    try {
      // Convert EPUB if needed
      if (needsConversion) {
        progressFill.style.backgroundColor = "#9b59b6"; // Purple for conversion
        progressText.textContent = `Converting ${file.name} (${currentIndex + 1}/${files.length})...`;

        // Clear log for single file mode, or just add separator for batch mode
        if (!useBatchLog) {
          clearLog();
          showLog();
        }

        // Snapshot before the `file =` reassignment below, which swaps in the
        // converted blob and makes file.size point at the optimised size.
        const origFileSize = file.size;

        try {
          const convertedBlob = await convertEpubFile(file, (percent) => {
            // Pass current quality setting to converter
            progressFill.style.width = percent * 0.5 + "%"; // Conversion takes first 50%
          });

          // Create new File from converted blob
          file = new File([convertedBlob], file.name, { type: "application/epub+zip" });
          progressFill.style.backgroundColor = "#27ae60"; // Back to green for upload
          conversionSucceeded = true;
          convOriginalSize = origFileSize;
          convNewSize = convertedBlob.size;
        } catch (convError) {
          if (operationCancelled) {
            if (uploadGeneration === myGeneration) restoreAfterCancel();
            return;
          }
          console.error("Conversion error:", convError);
          // Log the error
          logError(`Conversion failed: ${convError.message}`);
          log("Uploading original file instead...", "warning", "INFO");
          conversionFailed = true;

          // In single file mode, export error log
          if (!useBatchLog && exportLogCheckbox && exportLogCheckbox.checked) {
            setTimeout(() => {
              exportLogToFile(null, false); // isBatch = false for single file
            }, 100);
          }

          // If conversion fails, try uploading original file
          progressText.textContent = `Conversion failed, uploading original ${file.name}...`;
          progressFill.style.backgroundColor = "#e67e22"; // Orange for fallback
          // Reset progress bar to 0% for original file upload
          progressFill.style.width = "0%";
        }
      }

      if (useWebSocket) {
        await uploadFileWebSocket(file, onProgress, null, null);
      } else {
        await uploadFileHTTP(file, onProgress, null, null);
      }
      // Ensure progress bar shows 100% before moving to next file
      progressFill.style.width = "100%";
      progressText.textContent = `Upload complete: ${file.name}`;
      onComplete();
    } catch (error) {
      if (operationCancelled) {
        if (uploadGeneration === myGeneration) restoreAfterCancel();
        return;
      }
      console.error("Upload error:", error);
      // Log upload error if conversion succeeded but upload failed
      if (conversionSucceeded) {
        logError(`Upload failed: ${error.message}`);
      }

      if (useWebSocket && error.message === "WebSocket connection failed") {
        // Fall back to HTTP for all subsequent uploads
        console.log("WebSocket failed, falling back to HTTP");
        useWebSocket = false;
        // Retry this file with HTTP
        try {
          await uploadFileHTTP(file, onProgress, null, null);
          onComplete();
        } catch (httpError) {
          onError(httpError.message);
        }
      } else {
        onError(error.message);
      }
    }
  }

  uploadNextFile();
}

function showFailedUploadsBanner() {
  const banner = document.getElementById("failedUploadsBanner");
  const filesList = document.getElementById("failedFilesList");

  filesList.innerHTML = "";

  failedUploadsGlobal.forEach((failedFile, index) => {
    const item = document.createElement("div");
    item.className = "failed-file-item";
    item.innerHTML = `
      <div class="failed-file-info">
        <div class="failed-file-name">📄 ${escapeHtml(failedFile.name)}</div>
        <div class="failed-file-error">Error: ${escapeHtml(failedFile.error)}</div>
      </div>
      <button class="retry-btn" onclick="retrySingleUpload(${index})">Retry</button>
    `;
    filesList.appendChild(item);
  });

  // Ensure retry all button is visible
  const retryAllBtn = banner.querySelector(".retry-all-btn");
  if (retryAllBtn) retryAllBtn.style.display = "";

  banner.classList.add("show");
}

function dismissFailedUploads() {
  const banner = document.getElementById("failedUploadsBanner");
  banner.classList.remove("show");
  failedUploadsGlobal = [];
}

function retrySingleUpload(index) {
  const failedFile = failedUploadsGlobal[index];
  if (!failedFile) return;

  // Create a DataTransfer to set the file input
  const dt = new DataTransfer();
  dt.items.add(failedFile.file);

  const fileInput = document.getElementById("fileInput");
  fileInput.files = dt.files;

  // Remove this file from failed list
  failedUploadsGlobal.splice(index, 1);

  // If no more failed files, hide banner
  if (failedUploadsGlobal.length === 0) {
    dismissFailedUploads();
  }

  // Open modal and trigger upload
  openUploadModal();
  validateFile();
}

function retryAllFailedUploads() {
  if (failedUploadsGlobal.length === 0) return;

  // Create a DataTransfer with all failed files
  const dt = new DataTransfer();
  failedUploadsGlobal.forEach((failedFile) => {
    dt.items.add(failedFile.file);
  });

  const fileInput = document.getElementById("fileInput");
  fileInput.files = dt.files;

  // Clear failed files list
  failedUploadsGlobal = [];
  dismissFailedUploads();

  // Open modal and trigger upload
  openUploadModal();
  validateFile();
}

const SAFE_FILE_NAME_PATTERN = /^(?!\.{1,2}$)[^"*:<>?\/\\|]+$/;
const SAFE_FILE_NAME_RULE_MESSAGE = 'cannot contain " * : < > ? / \\ | and must not be . or ..';

function isSafeFileName(name) {
  return SAFE_FILE_NAME_PATTERN.test(name);
}

function createFolder() {
  const folderName = document.getElementById("folderName").value.trim();

  if (!folderName) {
    alert("Please enter a folder name!");
    return;
  }

  if (!isSafeFileName(folderName)) {
    alert("Folder name " + SAFE_FILE_NAME_RULE_MESSAGE);
    return;
  }

  const formData = new FormData();
  formData.append("name", folderName);
  formData.append("path", currentPath);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/mkdir", true);

  xhr.onload = function () {
    if (xhr.status === 200) {
      window.location.reload();
    } else {
      alert("Failed to create folder: " + xhr.responseText);
    }
  };

  xhr.onerror = function () {
    alert("Failed to create folder - network error");
  };

  xhr.send(formData);
}

// Rename functions
function openRenameModal(name, path) {
  document.getElementById("renameItemName").textContent = "📄 " + name;
  document.getElementById("renameItemPath").value = path;
  document.getElementById("renameNewName").value = name;
  document.getElementById("renameModal").classList.add("open");
  setTimeout(() => {
    const input = document.getElementById("renameNewName");
    input.focus();
    input.select();
  }, 50);
}

function closeRenameModal() {
  document.getElementById("renameModal").classList.remove("open");
}

function confirmRename() {
  const path = document.getElementById("renameItemPath").value;
  const newName = document.getElementById("renameNewName").value.trim();

  if (!newName) {
    alert("Please enter a new name.");
    return;
  }
  if (!isSafeFileName(newName)) {
    alert("File name " + SAFE_FILE_NAME_RULE_MESSAGE);
    return;
  }

  const formData = new FormData();
  formData.append("path", path);
  formData.append("name", newName);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/rename", true);

  xhr.onload = function () {
    if (xhr.status === 200) {
      window.location.reload();
    } else {
      alert("Failed to rename: " + xhr.responseText);
    }
    closeRenameModal();
  };

  xhr.onerror = function () {
    alert("Failed to rename - network error");
    closeRenameModal();
  };

  xhr.send(formData);
}

// Move functions
function normalizePath(path) {
  if (!path) return "/";
  let normalized = path.trim();
  if (!normalized.startsWith("/")) normalized = "/" + normalized;
  if (normalized.length > 1 && normalized.endsWith("/")) {
    normalized = normalized.slice(0, -1);
  }
  return normalized;
}

function getParentPath(path) {
  const normalized = normalizePath(path);
  if (normalized === "/") return "/";
  const idx = normalized.lastIndexOf("/");
  return idx <= 0 ? "/" : normalized.slice(0, idx);
}

async function loadMoveFolderOptions() {
  const options = new Set();
  options.add("/");
  const parent = getParentPath(currentPath);
  if (parent) options.add(parent);

  async function fetchFolders(path) {
    try {
      const response = await fetch("/api/files?path=" + encodeURIComponent(path));
      if (!response.ok) return [];
      return await response.json();
    } catch (e) {
      return [];
    }
  }

  const rootFiles = await fetchFolders("/");
  rootFiles.forEach((file) => {
    if (file.isDirectory) {
      options.add("/" + file.name);
    }
  });

  if (currentPath !== "/") {
    const currentFiles = await fetchFolders(currentPath);
    currentFiles.forEach((file) => {
      if (file.isDirectory) {
        let folderPath = currentPath;
        if (!folderPath.endsWith("/")) folderPath += "/";
        folderPath += file.name;
        options.add(folderPath);
      }
    });
  }

  const dataList = document.getElementById("moveFolderOptions");
  dataList.innerHTML = "";
  Array.from(options)
    .sort()
    .forEach((path) => {
      const option = document.createElement("option");
      option.value = path;
      dataList.appendChild(option);
    });
}

function openMoveModal(name, path) {
  document.getElementById("moveItemName").textContent = "📄 " + name;
  document.getElementById("moveItemPath").value = path;
  document.getElementById("moveDestPath").value = currentPath === "/" ? "/" : currentPath;
  document.getElementById("moveModal").classList.add("open");
  loadMoveFolderOptions();
  setTimeout(() => {
    document.getElementById("moveDestPath").focus();
  }, 50);
}

function closeMoveModal() {
  document.getElementById("moveModal").classList.remove("open");
}

function confirmMove() {
  const path = document.getElementById("moveItemPath").value;
  const destPath = normalizePath(document.getElementById("moveDestPath").value);

  if (!destPath) {
    alert("Please enter a destination folder.");
    return;
  }

  const formData = new FormData();
  formData.append("path", path);
  formData.append("dest", destPath);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/move", true);

  xhr.onload = function () {
    if (xhr.status === 200) {
      window.location.reload();
    } else {
      alert("Failed to move: " + xhr.responseText);
    }
    closeMoveModal();
  };

  xhr.onerror = function () {
    alert("Failed to move - network error");
    closeMoveModal();
  };

  xhr.send(formData);
}
hydrate();
