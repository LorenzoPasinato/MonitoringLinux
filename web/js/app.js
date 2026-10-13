async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.status || `HTTP ${response.status}`);
  return data;
}

let currentTargets = [];

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (character) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;'
  })[character]);
}

function renderTargets(targets) {
  const tbody = document.getElementById('targets-body');
  if (!tbody) return;

  currentTargets = targets || [];
  tbody.innerHTML = (targets || []).map((target) => {
    const needsPassword = ['password required', 'authentication failed'].includes(target.last_status);
    const statusClass = target.last_status === 'active' ? 'status-online' : (needsPassword ? 'status-auth' : 'status-offline');
    return `
      <tr>
        <td><strong>${escapeHtml(target.name)}</strong><br><span class="muted">${escapeHtml(target.ip)}</span></td>
        <td>${escapeHtml(target.username)}</td>
        <td>${escapeHtml(target.services)}</td>
        <td class="${statusClass}">${escapeHtml(target.last_status || 'pending')}</td>
        <td>${escapeHtml(target.last_checked_at || 'never')}</td>
        <td>
          <div class="target-actions">
            <button type="button" class="button-secondary button-small" data-action="edit" data-target-id="${target.id}">Edit</button>
            <button type="button" class="button-danger button-small" data-action="delete" data-target-id="${target.id}">Delete</button>
          </div>
          ${needsPassword ? `
            <form class="password-form" data-password-form="${target.id}">
              <input type="password" data-password-input="${target.id}" autocomplete="off" placeholder="SSH password" aria-label="SSH password for ${escapeHtml(target.name)}" required>
              <button type="submit" class="button-small">Connect</button>
            </form>
          ` : ''}
        </td>
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
        <td>${escapeHtml(check.name || check.ip)}</td>
        <td>${escapeHtml(check.service_name)}</td>
        <td class="${statusClass}">${check.active ? 'active' : 'inactive'}</td>
        <td>${escapeHtml(check.checked_at)}</td>
      </tr>
    `;
  }).join('');
}

async function loadStatus() {
  try {
    const activePasswordInput = document.activeElement?.matches('[data-password-input]') ? {
      targetId: document.activeElement.dataset.passwordInput,
      value: document.activeElement.value
    } : null;
    const targetsData = await fetchJson('/api/targets');
    renderTargets(targetsData.targets || []);
    if (activePasswordInput) {
      const passwordInput = document.querySelector(`[data-password-input="${activePasswordInput.targetId}"]`);
      if (passwordInput) {
        passwordInput.value = activePasswordInput.value;
        passwordInput.focus();
      }
    }

    const checksData = await fetchJson('/api/checks');
    renderChecks(checksData.checks || []);
  } catch (error) {
    console.error('Unable to load monitoring data:', error);
  }
}

const targetForm = document.getElementById('target-form');
const targetMessage = document.getElementById('target-message');

function resetTargetForm() {
  targetForm.reset();
  targetForm.dataset.targetId = '';
  document.getElementById('username').value = 'root';
  document.getElementById('port').value = '22';
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

document.getElementById('targets-body')?.addEventListener('click', async (event) => {
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

document.getElementById('targets-body')?.addEventListener('submit', async (event) => {
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

setInterval(loadStatus, 10000);
loadStatus();

