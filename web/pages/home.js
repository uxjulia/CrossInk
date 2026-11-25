async function fetchStatus() {
      try {
        const response = await fetch('/api/status');
        if (!response.ok) {
          throw new Error('Failed to fetch status: ' + response.status + ' ' + response.statusText);
        }
        const data = await response.json();
        document.getElementById('version').textContent = data.version || 'N/A';
        document.getElementById('serial').textContent = data.serial || 'N/A';
        document.getElementById('ip-address').textContent = data.ip || 'N/A';
        document.getElementById('free-heap').textContent = Number.isFinite(data.freeHeap)
          ? data.freeHeap.toLocaleString() + ' bytes'
          : 'N/A';
      } catch (error) {
        console.error('Error fetching status:', error);
      }
    }

    // Fetch status on page load
    fetchStatus();
