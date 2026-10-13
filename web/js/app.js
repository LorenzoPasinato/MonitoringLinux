async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.status || `HTTP ${response.status}`);
  return data;
}

let currentTargets = [];
let statusRefreshInProgress = false;
let lastRenderedState = '';

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (character) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;'
  })[character]);
}

function renderMachines(targets, checks) {
  const machineList = document.getElementById('machines-list');
  if (!machineList) return;

  currentTargets = targets || [];
  const checksByTarget = new Map();
  (checks || []).forEach((check) => {
    const targetId = String(check.target_id);
    if (!checksByTarget.has(targetId)) checksByTarget.set(targetId, new Map());
    checksByTarget.get(targetId).set(check.service_name, check);
  });

  machineList.innerHTML = currentTargets.map((target) => {
    const configuredServices = target.services.split(/[;,]/).map((service) => service.trim()).filter(Boolean);
    const machineChecks = checksByTarget.get(String(target.id)) || new Map();
    const services = configuredServices.map((serviceName) => {
      const check = machineChecks.get(serviceName);
      const authenticationNeeded = check && ['password required', 'authentication failed'].includes(check.status_text);
      const statusClass = !check ? 'status-pending' : (check.active ? 'status-online' : (authenticationNeeded ? 'status-auth' : 'status-offline'));
      const statusText = !check ? 'Waiting for first check' : (authenticationNeeded ? 'Authentication required' : check.status_text);
      return `
        <div class="service-row">
          <span class="service-name">${escapeHtml(serviceName)}</span>
          <span class="${statusClass}">${escapeHtml(statusText)}</span>
          <time>${escapeHtml(check?.checked_at || 'Not checked yet')}</time>
        </div>
      `;
    }).join('');
    const authNeeded = configuredServices.some((serviceName) => {
      const check = machineChecks.get(serviceName);
      return check && ['password required', 'authentication failed'].includes(check.status_text);
    });

    return `
      <article class="machine-card">
        <header class="machine-header">
          <div>
            <h3>${escapeHtml(target.name)}</h3>
            <p>${escapeHtml(target.ip)} · ${escapeHtml(target.username)}:${target.port}</p>
          </div>
          <div class="target-actions">
            <button type="button" class="button-secondary button-small" data-action="edit" data-target-id="${target.id}">Edit</button>
            <button type="button" class="button-danger button-small" data-action="delete" data-target-id="${target.id}">Delete</button>
          </div>
        </header>
        <div class="machine-services">
          <h4>Services</h4>
          ${services || '<p class="muted">No services configured</p>'}
        </div>
        ${authNeeded ? `
          <form class="password-form" data-password-form="${target.id}">
            <label>SSH password
              <input type="password" data-password-input="${target.id}" autocomplete="off" placeholder="Enter password" aria-label="SSH password for ${escapeHtml(target.name)}" required>
            </label>
            <button type="submit" class="button-small">Connect</button>
          </form>
        ` : ''}
      </article>
    `;
  }).join('');
}

async function loadStatus() {
  if (statusRefreshInProgress) return;
  statusRefreshInProgress = true;
  try {
    const [targetsData, checksData] = await Promise.all([
      fetchJson('/api/targets'),
      fetchJson('/api/checks')
    ]);
    const targets = targetsData.targets || [];
    const checks = checksData.checks || [];
    const nextState = JSON.stringify([targets, checks]);
    if (nextState !== lastRenderedState) {
      const activePasswordInput = document.activeElement?.matches('[data-password-input]') ? {
        targetId: document.activeElement.dataset.passwordInput,
        value: document.activeElement.value
      } : null;
      renderMachines(targets, checks);
      lastRenderedState = nextState;
      if (activePasswordInput) {
        const passwordInput = document.querySelector(`[data-password-input="${activePasswordInput.targetId}"]`);
        if (passwordInput) {
          passwordInput.value = activePasswordInput.value;
          passwordInput.focus();
        }
      }
    }
  } catch (error) {
    console.error('Unable to load monitoring data:', error);
  } finally {
    statusRefreshInProgress = false;
  }
}

const targetForm = document.getElementById('target-form');
const targetMessage = document.getElementById('target-message');

function resetTargetForm() {
  targetForm.reset();
  targetForm.dataset.targetId = '';
  document.getElementById('username').value = 'root';
  document.getElementById('port').value = '22';
  document.getElementById('password').placeholder = 'Optional; kept only while the app runs';
  document.getElementById('target-form-title').textContent = 'Add target VM';
  document.getElementById('save-target').textContent = 'Save target';
  document.getElementById('cancel-edit').hidden = true;
}

targetForm?.addEventListener('submit', async (event) => {
  event.preventDefault();

  const name = document.getElementById('name').value.trim();
  const ip = document.getElementById('ip').value.trim();
  const username = document.getElementById('username').value.trim();
  const port = document.getElementById('port').value || '22';
  const password = document.getElementById('password').value;
  const services = document.getElementById('services').value.trim();

  if (!ip || !services) {
    return;
  }

  const formData = new URLSearchParams({
    name,
    ip,
    username: username || 'root',
    port,
    services
  });
  if (password) formData.set('password', password);

  try {
    const targetId = targetForm.dataset.targetId;
    await fetchJson(targetId ? `/api/targets/${targetId}` : '/api/targets', {
      method: targetId ? 'PUT' : 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: formData.toString()
    });
    resetTargetForm();
    targetMessage.textContent = 'Machine saved';
    targetMessage.className = '';
    await loadStatus();
  } catch (error) {
    targetMessage.textContent = `Unable to save machine: ${error.message}`;
    targetMessage.className = 'form-message-error';
  }
});

document.getElementById('cancel-edit')?.addEventListener('click', resetTargetForm);

document.getElementById('machines-list')?.addEventListener('click', async (event) => {
  const button = event.target.closest('button[data-action]');
  if (!button) return;

  const targetId = Number(button.dataset.targetId);
  const target = currentTargets.find((item) => item.id === targetId);
  if (!target) return;

  if (button.dataset.action === 'edit') {
    targetForm.dataset.targetId = String(target.id);
    document.getElementById('name').value = target.name;
    document.getElementById('ip').value = target.ip;
    document.getElementById('username').value = target.username;
    document.getElementById('port').value = String(target.port);
    document.getElementById('services').value = target.services;
    document.getElementById('password').value = '';
    document.getElementById('password').placeholder = 'Leave blank to keep current password';
    document.getElementById('target-form-title').textContent = `Edit ${target.name}`;
    document.getElementById('save-target').textContent = 'Save changes';
    document.getElementById('cancel-edit').hidden = false;
    document.getElementById('name').focus();
    return;
  }

  if (button.dataset.action === 'delete' && window.confirm(`Delete ${target.name} and its check history?`)) {
    try {
      await fetchJson(`/api/targets/${target.id}`, { method: 'DELETE' });
      if (targetForm.dataset.targetId === String(target.id)) resetTargetForm();
      await loadStatus();
    } catch (error) {
      window.alert(`Unable to delete machine: ${error.message}`);
    }
  }
});

document.getElementById('machines-list')?.addEventListener('submit', async (event) => {
  const form = event.target.closest('form[data-password-form]');
  if (!form) return;
  event.preventDefault();

  const targetId = form.dataset.passwordForm;
  const passwordInput = form.querySelector('input[type="password"]');
  const password = passwordInput.value;
  passwordInput.value = '';
  try {
    await fetchJson(`/api/targets/${targetId}/password`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ password }).toString()
    });
    await loadStatus();
  } catch (error) {
    const message = document.createElement('span');
    message.className = 'form-message-error';
    message.textContent = 'Password was not accepted by the server';
    form.after(message);
  }
});

setInterval(loadStatus, 2500);
loadStatus();

