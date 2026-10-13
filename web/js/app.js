async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.status || `HTTP ${response.status}`);
  return data;
}

let currentTargets = [];
let statusRefreshInProgress = false;
let lastRenderedState = '';
let pendingRemoteAction = null;

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
          <div class="service-actions">
            <button type="button" class="button-secondary" data-action="restart-service" data-target-id="${target.id}" data-service="${escapeHtml(serviceName)}">Restart</button>
            <button type="button" class="button-danger" data-action="stop-service" data-target-id="${target.id}" data-service="${escapeHtml(serviceName)}">Stop</button>
          </div>
        </div>
      `;
    }).join('');
    const authNeeded = configuredServices.some((serviceName) => {
      const check = machineChecks.get(serviceName);
      return check && ['password required', 'authentication failed'].includes(check.status_text);
    });
    const checkResults = [...machineChecks.values()];
    const sshConnected = checkResults.some((check) => ['active', 'inactive or not found'].includes(check.status_text));
    const needsAuthentication = checkResults.some((check) => ['password required', 'authentication failed'].includes(check.status_text));
    const connectionState = sshConnected ? 'connected' : (needsAuthentication ? 'authentication' : (checkResults.length ? 'disconnected' : 'pending'));
    const connectionLabel = {
      connected: 'SSH connected',
      authentication: 'Authentication needed',
      disconnected: 'No connection',
      pending: 'Checking connection'
    }[connectionState];

    return `
      <article class="machine-card">
        <header class="machine-header">
          <div>
            <div class="machine-title-row">
              <span class="connection-state is-${connectionState}" role="img" title="${connectionLabel}" aria-label="${connectionLabel}">
                <span class="connection-led" aria-hidden="true"></span>
              </span>
              <h3>${escapeHtml(target.name)}</h3>
              <div class="machine-title-actions">
                <button type="button" class="button-secondary button-icon" data-action="edit" data-target-id="${target.id}" aria-label="Edit ${escapeHtml(target.name)}" title="Edit machine">&#9998;</button>
                <button type="button" class="button-danger button-icon" data-action="delete" data-target-id="${target.id}" aria-label="Delete ${escapeHtml(target.name)}" title="Delete machine">&#x1F5D1;&#xFE0E;</button>
              </div>
            </div>
            <p>${escapeHtml(target.username)}@${escapeHtml(target.ip)}&nbsp;&nbsp; port:${target.port}</p>
          </div>
          <div class="target-actions">
            <button type="button" class="button-secondary button-small" data-action="terminal" data-target-id="${target.id}">Terminal</button>
            <button type="button" class="button-secondary button-small" data-action="reboot" data-target-id="${target.id}">Reboot</button>
            <button type="button" class="button-danger button-small" data-action="poweroff" data-target-id="${target.id}">Shutdown</button>
          </div>
        </header>
        <div class="machine-services">
          <h4>Services</h4>
          ${services || '<p class="muted">No services configured</p>'}
        </div>
        <p class="machine-message" data-machine-message="${target.id}" role="status" aria-live="polite"></p>
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
const deleteDialog = document.getElementById('delete-dialog');
const remoteActionDialog = document.getElementById('remote-action-dialog');
let pendingDeleteTargetId = null;
const terminalSessions = new Map();
let nextTerminalId = 1;

function openTerminal(target) {
  const terminalId = `terminal-${nextTerminalId++}`;
  const dialog = document.createElement('dialog');
  dialog.className = 'terminal-window';
  dialog.setAttribute('aria-labelledby', `${terminalId}-title`);

  const header = document.createElement('header');
  header.className = 'terminal-header';
  const headingGroup = document.createElement('div');
  const title = document.createElement('h2');
  title.id = `${terminalId}-title`;
  title.textContent = `${target.name} terminal`;
  const hostLabel = document.createElement('p');
  hostLabel.textContent = `${target.username}@${target.ip} port:${target.port}`;
  headingGroup.append(title, hostLabel);

  const windowActions = document.createElement('div');
  windowActions.className = 'terminal-window-actions';
  const minimizeButton = document.createElement('button');
  minimizeButton.type = 'button';
  minimizeButton.className = 'button-secondary window-control';
  minimizeButton.setAttribute('aria-label', `Minimize ${target.name} terminal`);
  minimizeButton.title = 'Minimize';
  minimizeButton.textContent = '\u2212';
  const closeButton = document.createElement('button');
  closeButton.type = 'button';
  closeButton.className = 'button-secondary window-control';
  closeButton.setAttribute('aria-label', `Close ${target.name} terminal`);
  closeButton.title = 'Close';
  closeButton.textContent = '\u00d7';
  windowActions.append(minimizeButton, closeButton);
  header.append(headingGroup, windowActions);

  const screen = document.createElement('div');
  screen.className = 'terminal-screen';
  screen.setAttribute('aria-label', `Interactive SSH terminal for ${target.name}`);
  const message = document.createElement('p');
  message.className = 'terminal-message';
  message.setAttribute('role', 'status');
  message.setAttribute('aria-live', 'polite');
  message.textContent = 'Connecting over SSH...';
  dialog.append(header, screen, message);
  document.getElementById('terminal-windows').append(dialog);
  dialog.show();
  const offset = (terminalSessions.size % 5) * 28;
  dialog.style.left = `${Math.max(8, window.innerWidth - dialog.offsetWidth - 24 - offset)}px`;
  dialog.style.top = `${Math.max(8, 24 + offset)}px`;

  const terminal = new window.Terminal({
    cursorBlink: true,
    fontFamily: 'Cascadia Code, Consolas, monospace',
    fontSize: 13,
    theme: { background: '#080a08', foreground: '#dce5dc', cursor: '#a3d977', selectionBackground: '#394737' }
  });
  const fitAddon = new window.FitAddon.FitAddon();
  terminal.loadAddon(fitAddon);
  terminal.open(screen);
  fitAddon.fit();

  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const socket = new WebSocket(`${protocol}//${window.location.host}/api/targets/${target.id}/terminal`);
  const restoreButton = document.createElement('button');
  restoreButton.type = 'button';
  restoreButton.className = 'button-secondary restore-terminal-button';
  restoreButton.textContent = `${target.name} · ${target.ip}`;
  restoreButton.title = `Restore ${target.name} terminal`;
  restoreButton.hidden = true;
  document.getElementById('terminal-taskbar').append(restoreButton);
  const session = { terminalId, target, dialog, header, message, terminal, fitAddon, socket, restoreButton, minimized: false, closed: false };
  terminalSessions.set(terminalId, session);
  updateTerminalTaskbar();

  const resizeObserver = new ResizeObserver(() => {
    if (dialog.open) fitAddon.fit();
  });
  resizeObserver.observe(dialog);
  session.resizeObserver = resizeObserver;

  socket.addEventListener('open', () => {
    message.textContent = 'Connected';
    terminal.focus();
    socket.send(JSON.stringify({ type: 'resize', cols: terminal.cols, rows: terminal.rows }));
  });
  socket.addEventListener('message', (event) => terminal.write(event.data));
  socket.addEventListener('error', () => {
    message.textContent = 'SSH terminal connection failed';
  });
  socket.addEventListener('close', () => {
    message.textContent = 'SSH session closed';
  });
  terminal.onData((data) => {
    if (socket.readyState === WebSocket.OPEN) socket.send(data);
  });
  terminal.onResize(({ cols, rows }) => {
    if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ type: 'resize', cols, rows }));
  });

  const closeSession = () => closeTerminal(terminalId);
  closeButton.addEventListener('click', closeSession);
  minimizeButton.addEventListener('click', () => minimizeTerminal(terminalId));
  restoreButton.addEventListener('click', () => restoreTerminal(terminalId));
  dialog.addEventListener('cancel', (event) => {
    event.preventDefault();
    closeSession();
  });
  dialog.addEventListener('close', () => {
    if (!session.minimized && !session.closed) closeSession();
  });
  enableTerminalDragging(session);
  requestAnimationFrame(() => fitAddon.fit());
}

function updateTerminalTaskbar() {
  const taskbar = document.getElementById('terminal-taskbar');
  taskbar.hidden = ![...terminalSessions.values()].some((session) => session.minimized);
}

function minimizeTerminal(terminalId) {
  const session = terminalSessions.get(terminalId);
  if (!session || session.closed || session.minimized) return;
  session.minimized = true;
  session.dialog.close();
  session.restoreButton.hidden = false;
  updateTerminalTaskbar();
}

function restoreTerminal(terminalId) {
  const session = terminalSessions.get(terminalId);
  if (!session || session.closed || !session.minimized) return;
  session.minimized = false;
  session.restoreButton.hidden = true;
  session.dialog.show();
  requestAnimationFrame(() => {
    session.fitAddon.fit();
    session.terminal.focus();
  });
  updateTerminalTaskbar();
}

function closeTerminal(terminalId) {
  const session = terminalSessions.get(terminalId);
  if (!session || session.closed) return;
  session.closed = true;
  session.socket.close();
  session.terminal.dispose();
  session.resizeObserver?.disconnect();
  session.restoreButton.remove();
  if (session.dialog.open) session.dialog.close();
  session.dialog.remove();
  terminalSessions.delete(terminalId);
  updateTerminalTaskbar();
}

function enableTerminalDragging(session) {
  session.header.addEventListener('pointerdown', (event) => {
    if (event.button !== 0 || event.target.closest('button') || !session.dialog.open) return;
    const bounds = session.dialog.getBoundingClientRect();
    const offsetX = event.clientX - bounds.left;
    const offsetY = event.clientY - bounds.top;
    session.header.setPointerCapture(event.pointerId);

    const moveWindow = (moveEvent) => {
      if (!session.header.hasPointerCapture(moveEvent.pointerId)) return;
      const maxLeft = Math.max(0, window.innerWidth - session.dialog.offsetWidth);
      const maxTop = Math.max(0, window.innerHeight - 48);
      session.dialog.style.left = `${Math.max(0, Math.min(maxLeft, moveEvent.clientX - offsetX))}px`;
      session.dialog.style.top = `${Math.max(0, Math.min(maxTop, moveEvent.clientY - offsetY))}px`;
    };
    const stopMoving = (upEvent) => {
      if (session.header.hasPointerCapture(upEvent.pointerId)) session.header.releasePointerCapture(upEvent.pointerId);
      session.header.removeEventListener('pointermove', moveWindow);
      session.header.removeEventListener('pointerup', stopMoving);
      session.header.removeEventListener('pointercancel', stopMoving);
    };
    session.header.addEventListener('pointermove', moveWindow);
    session.header.addEventListener('pointerup', stopMoving);
    session.header.addEventListener('pointercancel', stopMoving);
  });
}

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

async function executeRemoteAction(target, action, service) {
  const message = document.querySelector(`[data-machine-message="${target.id}"]`);
  if (message) message.textContent = 'Sending remote command...';
  try {
    const response = await fetch(`/api/targets/${target.id}/actions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ action, service }).toString()
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.output || result.status || `HTTP ${response.status}`);
    await loadStatus();
    const updatedMessage = document.querySelector(`[data-machine-message="${target.id}"]`);
    if (updatedMessage) updatedMessage.textContent = `${result.status}: ${result.output || action}`;
  } catch (error) {
    if (message) message.textContent = `Command failed: ${error.message}`;
  }
}

function confirmRemoteAction(target, action, service = '') {
  pendingRemoteAction = { target, action, service };
  const actionNames = {
    'stop-service': `Stop ${service} on ${target.name}`,
    reboot: `Reboot ${target.name}`,
    poweroff: `Shut down the entire machine ${target.name}`
  };
  document.getElementById('remote-action-title').textContent = 'Confirm remote action';
  document.getElementById('remote-action-message').textContent = `${actionNames[action] || action}? This command will be sent over SSH.`;
  document.getElementById('confirm-remote-action').textContent = action === 'poweroff' ? 'Shutdown' : 'Confirm';
  remoteActionDialog.showModal();
}

document.getElementById('cancel-remote-action')?.addEventListener('click', () => remoteActionDialog.close());
document.getElementById('confirm-remote-action')?.addEventListener('click', async () => {
  const action = pendingRemoteAction;
  pendingRemoteAction = null;
  remoteActionDialog.close();
  if (action) await executeRemoteAction(action.target, action.action, action.service);
});
remoteActionDialog?.addEventListener('close', () => {
  pendingRemoteAction = null;
});

async function deleteTarget(targetId) {
  const target = currentTargets.find((item) => item.id === targetId);
  if (!target) return;

  try {
    await fetchJson(`/api/targets/${target.id}`, { method: 'DELETE' });
    if (targetForm.dataset.targetId === String(target.id)) resetTargetForm();
    await loadStatus();
  } catch (error) {
    window.alert(`Unable to delete machine: ${error.message}`);
  }
}

document.getElementById('cancel-delete')?.addEventListener('click', () => deleteDialog.close());
document.getElementById('confirm-delete')?.addEventListener('click', async () => {
  const targetId = pendingDeleteTargetId;
  pendingDeleteTargetId = null;
  deleteDialog.close();
  if (targetId !== null) await deleteTarget(targetId);
});

deleteDialog?.addEventListener('close', () => {
  pendingDeleteTargetId = null;
});

deleteDialog?.addEventListener('click', (event) => {
  if (event.target === deleteDialog) deleteDialog.close();
});

document.getElementById('machines-list')?.addEventListener('click', async (event) => {
  const button = event.target.closest('button[data-action]');
  if (!button) return;

  const targetId = Number(button.dataset.targetId);
  const target = currentTargets.find((item) => item.id === targetId);
  if (!target) return;

  if (button.dataset.action === 'terminal') {
    openTerminal(target);
    return;
  }

  if (button.dataset.action === 'restart-service') {
    await executeRemoteAction(target, button.dataset.action, button.dataset.service);
    return;
  }

  if (['stop-service', 'reboot', 'poweroff'].includes(button.dataset.action)) {
    confirmRemoteAction(target, button.dataset.action, button.dataset.service || '');
    return;
  }

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

  if (button.dataset.action === 'delete') {
    pendingDeleteTargetId = target.id;
    document.getElementById('delete-dialog-message').textContent = `Delete ${target.name} and its check history?`;
    deleteDialog.showModal();
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

