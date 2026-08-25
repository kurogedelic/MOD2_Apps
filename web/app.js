import { Picoboot } from 'https://cdn.jsdelivr.net/gh/piersfinlayson/picoflash@678355430aff0ee9efa6d552fb81832a91d89ef4/pkg/picoboot.js';
import { Target } from 'https://cdn.jsdelivr.net/gh/piersfinlayson/picoflash@678355430aff0ee9efa6d552fb81832a91d89ef4/pkg/target.js';
import { uf2ToFlashBuffer } from 'https://cdn.jsdelivr.net/gh/piersfinlayson/picoflash@678355430aff0ee9efa6d552fb81832a91d89ef4/js/uf2/uf2.js';

const appsEl = document.querySelector('#apps');
const searchEl = document.querySelector('#search');
const statusEl = document.querySelector('#status');
const warningEl = document.querySelector('#browser-warning');
const overlayEl = document.querySelector('#flash-overlay');
const stageEl = document.querySelector('#flash-stage');
const progressEl = document.querySelector('#progress-bar');

let apps = [];
let busy = false;

const supportsWebUsb = 'usb' in navigator;
if (!supportsWebUsb) warningEl.classList.remove('hidden');

function setStatus(text, kind = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${kind}`.trim();
}

function setStage(text, progress) {
  stageEl.textContent = text;
  progressEl.style.width = `${progress}%`;
}

function setBusy(value) {
  busy = value;
  document.querySelectorAll('.flash-btn').forEach((button) => {
    button.disabled = value || !supportsWebUsb;
  });
  overlayEl.classList.toggle('hidden', !value);
}

function firmwareUrl(app) {
  return `./firmware/${app.id}.uf2`;
}

function render(filter = '') {
  const needle = filter.trim().toLowerCase();
  const visible = apps.filter((app) => {
    const haystack = `${app.name} ${app.category} ${app.description}`.toLowerCase();
    return haystack.includes(needle);
  });

  appsEl.replaceChildren();
  if (!visible.length) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = 'No matching apps.';
    appsEl.append(empty);
    return;
  }

  for (const app of visible) {
    const card = document.createElement('article');
    card.className = 'card';

    const head = document.createElement('div');
    head.className = 'card-head';
    const title = document.createElement('h2');
    title.textContent = app.name;
    const tag = document.createElement('span');
    tag.className = `tag${app.tested ? '' : ' untested'}`;
    tag.textContent = app.tested ? app.category : `${app.category} · untested`;
    head.append(title, tag);

    const description = document.createElement('p');
    description.className = 'description';
    description.textContent = app.description;

    const actions = document.createElement('div');
    actions.className = 'actions';
    const flash = document.createElement('button');
    flash.className = 'flash-btn';
    flash.type = 'button';
    flash.textContent = 'Flash to MOD2';
    flash.disabled = busy || !supportsWebUsb;
    flash.addEventListener('click', () => flashApp(app));

    const download = document.createElement('a');
    download.className = 'download';
    download.href = firmwareUrl(app);
    download.download = `${app.id}.uf2`;
    download.title = 'Download UF2';
    download.setAttribute('aria-label', `Download ${app.name} UF2`);
    download.textContent = '↓';

    actions.append(flash, download);
    card.append(head, description, actions);
    appsEl.append(card);
  }
}

function validateFlashRange(address, data) {
  const flashStart = 0x10000000;
  const flashEnd = 0x10200000; // XIAO RP2350 has 2 MB flash
  if (address < flashStart || address >= flashEnd) {
    throw new Error(`Unexpected flash address 0x${address.toString(16)}`);
  }
  if (address + data.length > flashEnd) {
    throw new Error('Firmware is larger than the XIAO RP2350 flash range');
  }
  if (address % 4096 !== 0) {
    throw new Error('Firmware start address is not flash-sector aligned');
  }
}

async function flashApp(app) {
  if (busy || !supportsWebUsb) return;

  let picoboot = null;
  let connection = null;
  setBusy(true);
  setStatus(`Preparing ${app.name}…`);
  setStage(`Loading ${app.name} firmware…`, 10);

  try {
    const response = await fetch(firmwareUrl(app), { cache: 'no-store' });
    if (!response.ok) throw new Error(`Firmware file is unavailable (${response.status})`);

    const uf2 = new Uint8Array(await response.arrayBuffer());
    if (uf2.length === 0 || uf2.length % 512 !== 0) throw new Error('Invalid UF2 file size');

    const { address, data } = uf2ToFlashBuffer(uf2);
    validateFlashRange(address, data);

    setStage('Select “RP2 Boot” in the USB device chooser…', 22);
    picoboot = await Picoboot.requestDevice([new Target('RP2350')]);

    setStage('Connecting to MOD2…', 35);
    connection = await picoboot.connect();
    if (picoboot.getTarget().type !== 'RP2350') throw new Error('Selected device is not an RP2350');

    setStage(`Writing ${app.name}…`, 55);
    await picoboot.flashEraseAndWrite(address, data);

    setStage('Flash complete. Rebooting MOD2…', 92);
    try {
      await connection.reboot(100);
    } catch (error) {
      console.debug('Reboot disconnected before acknowledgement:', error);
    }

    try {
      await picoboot.disconnect();
    } catch (error) {
      console.debug('Disconnect after reboot:', error);
    }

    setStage('Done', 100);
    setStatus(`${app.name} flashed successfully`, 'success');
    await new Promise((resolve) => setTimeout(resolve, 650));
  } catch (error) {
    console.error(error);
    const cancelled = error?.name === 'NotFoundError' || /cancelled|no device selected/i.test(error?.message || '');
    setStatus(cancelled ? 'Device selection cancelled' : `Flash failed: ${error.message}`, cancelled ? '' : 'error');
    try {
      if (picoboot) await picoboot.disconnect();
    } catch (disconnectError) {
      console.debug(disconnectError);
    }
  } finally {
    setBusy(false);
    progressEl.style.width = '8%';
  }
}

searchEl.addEventListener('input', () => render(searchEl.value));

try {
  const response = await fetch('./manifest.json', { cache: 'no-store' });
  if (!response.ok) throw new Error(`manifest.json: ${response.status}`);
  apps = await response.json();
  render();
  if (!supportsWebUsb) setStatus('WebUSB unavailable', 'error');
} catch (error) {
  console.error(error);
  appsEl.innerHTML = '<div class="empty">Could not load the firmware manifest.</div>';
  setStatus('Manifest load failed', 'error');
}
