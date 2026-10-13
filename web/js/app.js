async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  return response.json();
}

function renderTargets(targets) {
  const tbody = document.getElementById('targets-body');
  if (!tbody) return;

  tbody.innerHTML = (targets || []).map((target) => {
    const statusClass = target.last_status === 'active' ? 'status-online' : 'status-offline';
    return `
      <tr>
        <td>${target.ip}</td>
        <td>${target.username}</td>
        <td>${target.services}</td>
        <td class="${statusClass}">${target.last_status || 'pending'}</td>
        <td>${target.last_checked_at || 'never'}</td>
      </tr>
    `;
  }).join('');
}

function renderChecks(checks) {
  const tbody = document.getElementById('checks-body');
  if (!tbody) return;

  tbody.innerHTML = (checks || []).map((check) => {
    const statusClass = check.active ? 'status-online' : 'status-offline';
    return `
      <tr>
        <td>${check.ip}</td>
        <td>${check.service_name}</td>
        <td class="${statusClass}">${check.active ? 'active' : 'inactive'}</td>
        <td>${check.checked_at}</td>
      </tr>
    `;
  }).join('');
}

async function loadStatus() {
  try {
    const targetsData = await fetchJson('/api/targets');
    renderTargets(targetsData.targets || []);

    const checksData = await fetchJson('/api/checks');
    renderChecks(checksData.checks || []);
  } catch (error) {
    console.error('Unable to load monitoring data:', error);
  }
}

document.getElementById('target-form')?.addEventListener('submit', async (event) => {
  event.preventDefault();

  const ip = document.getElementById('ip').value.trim();
  const username = document.getElementById('username').value.trim();
  const port = document.getElementById('port').value || '22';
  const services = document.getElementById('services').value.trim();

  if (!ip || !services) {
    return;
  }

  const formData = new URLSearchParams({
    ip,
    username: username || 'root',
    port,
    services
  });

  await fetch('/api/targets', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: formData.toString()
  });

  document.getElementById('target-form').reset();
  document.getElementById('username').value = 'root';
  document.getElementById('port').value = '22';
  loadStatus();
});

setInterval(loadStatus, 10000);
loadStatus();

