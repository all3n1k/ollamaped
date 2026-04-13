/**
 * Spider Robot Dashboard — frontend app
 * WebSocket client + Three.js 3D spider visualization
 */
import * as THREE from 'three';

// ============================================================
// WebSocket connection to brain server
// ============================================================
let ws = null;
let reconnectTimer = null;
const WS_URL = `ws://${location.host}/ws`;

function connect() {
  ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    clearTimeout(reconnectTimer);
    logEvent('Connected to brain server', 'info');
  };

  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if      (msg.type === 'state')     applyState(msg.data);
      else if (msg.type === 'brain_log') appendBrainLog(msg);
      // 'ping' keepalive — silently ignore
    } catch (_) {}
  };

  ws.onclose = () => {
    logEvent('WebSocket closed — reconnecting...', 'warn');
    reconnectTimer = setTimeout(connect, 2000);
  };

  ws.onerror = () => ws.close();
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN)
    ws.send(JSON.stringify(obj));
}

// ============================================================
// State → UI updates
// ============================================================
const $ = id => document.getElementById(id);

function applyState(s) {
  // Connection badge
  const badge = $('conn-badge');
  if (s.connected) {
    badge.textContent  = s.connection_type.toUpperCase();
    badge.className    = 'badge badge-on';
  } else {
    badge.textContent  = 'DISCONNECTED';
    badge.className    = 'badge badge-off';
  }

  // Header vitals
  $('battery-display').textContent = `⚡ ${s.voltage.toFixed(2)}V`;
  $('latency-display').textContent = `${s.rtt_ms.toFixed(0)} ms`;

  // Status grid
  $('stat-behavior').textContent = s.behavior;
  $('stat-voltage').textContent  = `${s.voltage.toFixed(2)}V`;
  $('stat-busy').textContent     = s.busy ? 'YES' : 'no';
  $('stat-busy').style.color     = s.busy ? 'var(--yellow)' : 'var(--green)';
  $('stat-conn').textContent     = s.connection_type;
  $('stat-lastseen').textContent = `${s.last_seen_ms}ms ago`;
  $('stat-rtt').textContent      = `${s.rtt_ms.toFixed(1)}ms`;

  // Behavior chip in 3D viewport
  $('behavior-label').textContent = s.behavior;
  updateBehaviorButtons(s.behavior);

  // Battery bar  — 6.5V empty, 8.4V full (2x 14500 fully charged)
  const pct = Math.max(0, Math.min(100, ((s.voltage - 6.5) / (8.4 - 6.5)) * 100));
  $('battery-bar').style.width = pct + '%';
  $('battery-pct').textContent = Math.round(pct) + '%';
  $('battery-bar').style.background = pct < 20
    ? 'var(--red)'
    : pct < 40
      ? 'var(--yellow)'
      : 'linear-gradient(90deg, var(--green), #84cc16)';

  // Event log — append new entries
  if (s.events?.length) appendEvents(s.events);

  // Update 3D model leg positions if available
  if (s.legs) updateLegs(s.legs);
}

// ============================================================
// Event log
// ============================================================
let _lastEventTs = 0;

function appendEvents(events) {
  const log = $('event-log');
  let added = false;
  for (const e of events) {
    if (e.ts <= _lastEventTs) continue;
    _lastEventTs = e.ts;
    const div = document.createElement('div');
    div.className = 'log-entry';
    const t = new Date(e.ts);
    const hms = t.toTimeString().slice(0, 8);
    div.innerHTML = `<span class="ts">${hms}</span><span class="msg">${escHtml(e.msg)}</span>`;
    log.appendChild(div);
    added = true;
  }
  if (added) log.scrollTop = log.scrollHeight;
}

function logEvent(msg, cls = '') {
  const log = $('event-log');
  const div = document.createElement('div');
  div.className = `log-entry ${cls}`;
  const hms = new Date().toTimeString().slice(0, 8);
  div.innerHTML = `<span class="ts">${hms}</span><span class="msg">${escHtml(msg)}</span>`;
  log.appendChild(div);
  log.scrollTop = log.scrollHeight;
}

function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

$('clear-log').addEventListener('click',   () => { $('event-log').innerHTML = ''; });
$('clear-brain').addEventListener('click', () => { $('brain-log').innerHTML = ''; });

// ============================================================
// Brain console
// ============================================================
const BRAIN_COLORS = {
  autonomy: '#a855f7',
  cmd:      '#22c55e',
  robot:    '#60a5fa',
  warn:     '#eab308',
  error:    '#ef4444',
  info:     '#94a3b8',
};

function appendBrainLog({ level = 'info', tag = '', msg = '' }) {
  const log  = $('brain-log');
  const div  = document.createElement('div');
  div.className = 'log-entry';
  const hms  = new Date().toTimeString().slice(0, 8);
  const col  = BRAIN_COLORS[level] || BRAIN_COLORS.info;
  const tagHtml = tag ? `<span style="color:${col};font-weight:700">[${escHtml(tag)}]</span> ` : '';
  div.innerHTML = `<span class="ts">${hms}</span>${tagHtml}<span class="msg">${escHtml(msg)}</span>`;
  log.appendChild(div);
  // Keep last 300 lines
  while (log.children.length > 300) log.removeChild(log.firstChild);
  log.scrollTop = log.scrollHeight;
}

// ============================================================
// Behavior buttons
// ============================================================
document.querySelectorAll('.behav-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    send({ action: 'behavior', state: btn.dataset.state });
  });
});

function updateBehaviorButtons(active) {
  document.querySelectorAll('.behav-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.state === active);
  });
}

// ============================================================
// Movement pad buttons
// ============================================================
document.querySelectorAll('.move-btn[data-cmd]').forEach(btn => {
  btn.addEventListener('mousedown', () => send(JSON.parse(btn.dataset.cmd)));
});

// ============================================================
// Keyboard controls  WASD + arrows + space
// ============================================================
const keyMap = {
  'ArrowUp':    { action:'walk', dir:'F' },
  'ArrowDown':  { action:'walk', dir:'B' },
  'ArrowLeft':  { action:'walk', dir:'L' },
  'ArrowRight': { action:'walk', dir:'R' },
  'KeyW':       { action:'walk', dir:'F' },
  'KeyS':       { action:'walk', dir:'B' },
  'KeyA':       { action:'walk', dir:'L' },
  'KeyD':       { action:'walk', dir:'R' },
  'Space':      { action:'stop' },
  'KeyH':       { action:'behavior', state:'happy' },
  'KeyI':       { action:'behavior', state:'idle' },
  'Escape':     { action:'stop' },
};

const heldKeys = new Set();
let walkRepeat = null;

document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  const cmd = keyMap[e.code];
  if (!cmd) return;
  e.preventDefault();
  if (heldKeys.has(e.code)) return;
  heldKeys.add(e.code);
  send(cmd);
  if (cmd.action === 'walk') {
    walkRepeat = setInterval(() => send(cmd), 400);
  }
});

document.addEventListener('keyup', e => {
  if (heldKeys.has(e.code)) {
    heldKeys.delete(e.code);
    clearInterval(walkRepeat);
    // Only auto-stop if it was a walk key
    const cmd = keyMap[e.code];
    if (cmd?.action === 'walk') send({ action:'stop' });
  }
});

// ============================================================
// Body sliders — debounced send
// ============================================================
let sliderTimer = null;
function onSliderChange() {
  clearTimeout(sliderTimer);
  sliderTimer = setTimeout(sendSliders, 80);
}

function sendSliders() {
  const bx = +$('body-x').value, by = +$('body-y').value, bz = +$('body-z').value;
  const rx = +$('rot-x').value,  ry = +$('rot-y').value,  rz = +$('rot-z').value;
  $('body-x-val').textContent = bx;
  $('body-y-val').textContent = by;
  $('body-z-val').textContent = bz;
  $('rot-x-val').textContent  = (rx/10).toFixed(1);
  $('rot-y-val').textContent  = (ry/10).toFixed(1);
  $('rot-z-val').textContent  = (rz/10).toFixed(1);
  if (bx || by || bz) send({ action:'body', x:bx, y:by, z:bz });
  if (rx || ry || rz) send({ action:'rotate', x:rx, y:ry, z:rz });
}

['body-x','body-y','body-z','rot-x','rot-y','rot-z'].forEach(id =>
  $(id).addEventListener('input', onSliderChange)
);

$('body-reset').addEventListener('click', () => {
  ['body-x','body-y','body-z'].forEach(id => $(id).value = 0);
  send({ action:'body', x:0, y:0, z:0 });
  sendSliders();
});

$('rot-reset').addEventListener('click', () => {
  ['rot-x','rot-y','rot-z'].forEach(id => $(id).value = 0);
  send({ action:'rotate', x:0, y:0, z:0 });
  sendSliders();
});

// ============================================================
// Three.js — 3D spider visualization
// ============================================================
const canvas  = $('spider-canvas');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
renderer.setPixelRatio(window.devicePixelRatio);
renderer.shadowMap.enabled = true;

const scene  = new THREE.Scene();
scene.background = new THREE.Color(0x0d0d10);
scene.fog = new THREE.FogExp2(0x0d0d10, 0.018);

const camera = new THREE.PerspectiveCamera(55, 1, 1, 1000);
camera.position.set(0, -220, 180);
camera.lookAt(0, 0, 0);

// Lighting
scene.add(new THREE.AmbientLight(0x334455, 1.2));
const dirLight = new THREE.DirectionalLight(0x8866ff, 2.5);
dirLight.position.set(100, -100, 200);
dirLight.castShadow = true;
scene.add(dirLight);
scene.add(new THREE.PointLight(0x7c3aed, 1.5, 300));

// Grid floor
const grid = new THREE.GridHelper(400, 20, 0x2a2a35, 0x1c1c22);
grid.position.y = -5;
scene.add(grid);

// Materials
const bodyMat = new THREE.MeshPhongMaterial({
  color: 0x7c3aed, emissive: 0x2d1058, shininess: 80,
});
const legMat = new THREE.MeshPhongMaterial({
  color: 0xa855f7, emissive: 0x3b0764, shininess: 60,
});
const jointMat = new THREE.MeshPhongMaterial({
  color: 0xc084fc, emissive: 0x581c87, shininess: 100,
});
const footMat = new THREE.MeshPhongMaterial({
  color: 0xe9d5ff, emissive: 0x1a0533, shininess: 120,
});

// Body
const bodyGeo = new THREE.BoxGeometry(70, 90, 18, 2, 2, 1);
const body    = new THREE.Mesh(bodyGeo, bodyMat);
body.castShadow = true;
scene.add(body);

// Helper: create a leg group with 3 segments
function makeLeg(xSign, ySign) {
  const group = new THREE.Group();

  // Hip joint
  const hip = new THREE.Mesh(new THREE.SphereGeometry(5, 8, 8), jointMat);
  hip.position.set(xSign * 35, ySign * 35, 0);
  group.add(hip);

  // Upper leg (femur)
  const femurGeo = new THREE.CylinderGeometry(2.5, 3, 55, 6);
  const femur    = new THREE.Mesh(femurGeo, legMat);
  femur.castShadow = true;

  // Knee joint
  const knee = new THREE.Mesh(new THREE.SphereGeometry(4, 8, 8), jointMat);

  // Lower leg (tibia)
  const tibiaGeo = new THREE.CylinderGeometry(2, 2.5, 70, 6);
  const tibia    = new THREE.Mesh(tibiaGeo, legMat);
  tibia.castShadow = true;

  // Foot
  const foot = new THREE.Mesh(new THREE.SphereGeometry(4.5, 8, 8), footMat);

  // These will be repositioned by updateLegs()
  group.add(femur);
  group.add(knee);
  group.add(tibia);
  group.add(foot);

  group.userData = { hip, femur, knee, tibia, foot, xSign, ySign };
  scene.add(group);
  return group;
}

// Leg origins match firmware (mm scaled to scene units / 1.0)
const legGroups = [
  makeLeg(-1,  1),   // leg1 front-left
  makeLeg(-1, -1),   // leg2 front-right (note: firmware y is reversed for right side)
  makeLeg( 1,  1),   // leg3 rear-left
  makeLeg( 1, -1),   // leg4 rear-right
];

// Default leg foot positions (boot points from firmware)
const defaultFeet = [
  { x: -90, y:  90, z: 0 },
  { x: -90, y: -90, z: 0 },
  { x:  90, y:  90, z: 0 },
  { x:  90, y: -90, z: 0 },
];

let currentFeet = defaultFeet.map(f => ({ ...f }));

function updateLegs(legsData) {
  for (let i = 1; i <= 4; i++) {
    const d = legsData[String(i)];
    if (d) currentFeet[i-1] = { x: d.x, y: d.y, z: d.z };
  }
}

function positionLegs() {
  // Scale: firmware mm → Three.js units 1:1
  for (let i = 0; i < 4; i++) {
    const lg   = legGroups[i];
    const { xSign, ySign, femur, knee, tibia, foot } = lg.userData;
    const foot3d = currentFeet[i];

    // Hip origin
    const hipX = xSign * 35;
    const hipY = ySign * 35;
    const hipZ = 0;

    // Foot target
    const fx = foot3d.x;
    const fy = foot3d.y;
    const fz = -foot3d.z; // firmware z=0 is ground, positive = up; Three.js z inverted

    // Simple 2-segment IK visualization: mid joint = midpoint raised
    const mx = (hipX + fx) * 0.5;
    const my = (hipY + fy) * 0.5;
    const mz = hipZ + 30;  // knee lifts above hip by 30 units

    // Position femur between hip and knee
    femur.position.set((hipX+mx)/2, (hipY+my)/2, (hipZ+mz)/2);
    femur.lookAt(mx, my, mz);
    femur.rotateX(Math.PI / 2);

    knee.position.set(mx, my, mz);

    // Position tibia between knee and foot
    tibia.position.set((mx+fx)/2, (my+fy)/2, (mz+fz)/2);
    tibia.lookAt(fx, fy, fz);
    tibia.rotateX(Math.PI / 2);

    foot.position.set(fx, fy, fz);
  }
}

// Body oscillation driven by behavior state
let bodyBobPhase = 0;
const behaviorBodyAnim = {
  idle:    { bobAmp: 3,  bobSpeed: 0.04, swayAmp: 2 },
  alert:   { bobAmp: 0,  bobSpeed: 0,    swayAmp: 0 },
  happy:   { bobAmp: 4,  bobSpeed: 0.12, swayAmp: 8 },
  curious: { bobAmp: 1,  bobSpeed: 0.03, swayAmp: 0 },
  sleepy:  { bobAmp: 1.5,bobSpeed: 0.02, swayAmp: 1 },
  sleep:   { bobAmp: 0,  bobSpeed: 0,    swayAmp: 0 },
};

let lastBehavior = 'idle';

function animate() {
  requestAnimationFrame(animate);

  // Resize canvas to panel size — skip if layout hasn't assigned dimensions yet
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (w > 0 && h > 0 && (renderer.domElement.width !== w || renderer.domElement.height !== h)) {
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }

  // Body animation
  bodyBobPhase += 0.04;
  const anim = behaviorBodyAnim[lastBehavior] ?? behaviorBodyAnim.idle;
  body.position.z = Math.sin(bodyBobPhase * anim.bobSpeed / 0.04) * anim.bobAmp;
  body.rotation.y = Math.sin(bodyBobPhase * 0.5) * (anim.swayAmp * 0.02);

  positionLegs();

  renderer.render(scene, camera);
}

// Keep lastBehavior in sync
setInterval(() => {
  const chip = $('behavior-label');
  if (chip) lastBehavior = chip.textContent.trim();
}, 200);

// ============================================================
// Orbit camera with mouse drag
// ============================================================
let isDragging = false, lastMX = 0, lastMY = 0;
let camTheta = Math.PI, camPhi = 0.8, camRadius = 280;

canvas.addEventListener('mousedown', e => { isDragging = true; lastMX = e.clientX; lastMY = e.clientY; });
window.addEventListener('mouseup',   () => { isDragging = false; });
window.addEventListener('mousemove', e => {
  if (!isDragging) return;
  const dx = e.clientX - lastMX, dy = e.clientY - lastMY;
  camTheta -= dx * 0.008;
  camPhi   = Math.max(0.15, Math.min(1.4, camPhi + dy * 0.006));
  lastMX = e.clientX; lastMY = e.clientY;
  camera.position.set(
    camRadius * Math.sin(camTheta) * Math.sin(camPhi),
    -camRadius * Math.cos(camPhi),
    camRadius * Math.cos(camTheta) * Math.sin(camPhi),
  );
  camera.lookAt(0, 0, 0);
});
canvas.addEventListener('wheel', e => {
  camRadius = Math.max(100, Math.min(600, camRadius + e.deltaY * 0.3));
  camera.position.setLength(camRadius);
});

// ============================================================
// Camera feed
// ============================================================
const CAMERA_BASE = 'http://192.168.1.210';
const camImg   = $('camera-img');
const camError = $('camera-error');

function camLoadStream() {
  camImg.style.display = 'block';
  camError.style.display = 'none';
  // Force reload by appending timestamp (breaks browser cache)
  camImg.src = CAMERA_BASE + '/stream?' + Date.now();
}

camImg.addEventListener('error', () => {
  camImg.style.display = 'none';
  camError.style.display = 'flex';
});

$('cam-refresh').addEventListener('click', camLoadStream);

$('cam-led-on').addEventListener('click', () => {
  fetch(CAMERA_BASE + '/led?state=on').catch(() => {});
});
$('cam-led-off').addEventListener('click', () => {
  fetch(CAMERA_BASE + '/led?state=off').catch(() => {});
});

// ============================================================
// Boot
// ============================================================
animate();
connect();
camLoadStream();
