/**
 * UniCore frontend logic — talks to the Express backend over REST and
 * renders the resource pool, department ledger, and event log.
 * No build step / framework required: open index.html directly, or serve
 * this folder with any static file server.
 */

const API_BASE = window.UNICORE_API_BASE || "http://localhost:4000";

const RESOURCE_LABELS = ["Classrooms", "Lab-PCs", "Printers", "Bandwidth", "Parking"];

const el = {
  statusPill: document.getElementById("statusPill"),
  resourceList: document.getElementById("resourceList"),
  ledgerBody: document.getElementById("ledgerBody"),
  logFeed: document.getElementById("logFeed"),
  reqDept: document.getElementById("reqDept"),
  relDept: document.getElementById("relDept"),
  reqVector: document.getElementById("reqVector"),
  relVector: document.getElementById("relVector"),
  requestForm: document.getElementById("requestForm"),
  releaseForm: document.getElementById("releaseForm"),
  btnSchedule: document.getElementById("btnSchedule"),
  btnSafety: document.getElementById("btnSafety"),
  btnDeadlock: document.getElementById("btnDeadlock"),
  btnReset: document.getElementById("btnReset"),
  consoleFeedback: document.getElementById("consoleFeedback"),
  apiBaseLabel: document.getElementById("apiBaseLabel"),
  connState: document.getElementById("connState"),
};

el.apiBaseLabel.textContent = API_BASE;

let deptNames = [];

/* ---------------------------------------------------------------------- */
/* Rendering                                                              */
/* ---------------------------------------------------------------------- */

function renderResources(resources) {
  el.resourceList.innerHTML = resources
    .map((r) => {
      const pct = Math.round(((r.total - r.available) / r.total) * 100);
      return `
        <div class="resource-item">
          <div class="resource-item__label">
            <strong>${r.name}</strong>
            <span class="resource-item__figures">${r.available}/${r.total} free</span>
          </div>
          <div class="gauge-track"><div class="gauge-fill" style="width:${pct}%"></div></div>
        </div>`;
    })
    .join("");
}

function renderLedger(departments) {
  el.ledgerBody.innerHTML = departments
    .map(
      (d) => `
      <tr>
        <td class="dept-name">${d.name}</td>
        <td>${d.priority}</td>
        <td class="vector">${d.allocation.join(" ")}</td>
        <td class="vector">${d.max.join(" ")}</td>
        <td class="vector">${d.need.join(" ")}</td>
        <td>${d.blocked ? '<span class="blocked-flag">BLOCKED</span>' : ""}</td>
      </tr>`
    )
    .join("");
}

function renderLog(log) {
  el.logFeed.innerHTML = log
    .map((entry) => {
      let cls = "";
      if (entry.message.startsWith("GRANT")) cls = "log-line--grant";
      else if (entry.message.startsWith("DENY")) cls = "log-line--deny";
      else if (entry.message.startsWith("WAIT")) cls = "log-line--wait";
      else if (entry.message.includes("confirmed safe")) cls = "log-line--safe";
      return `<div class="log-line ${cls}"><span class="log-line__time">${entry.time}</span>${escapeHtml(entry.message)}</div>`;
    })
    .join("");
}

function renderStatus(safe) {
  el.statusPill.textContent = safe ? "SAFE" : "UNSAFE";
  el.statusPill.className = "status-pill " + (safe ? "status-pill--safe" : "status-pill--unsafe");
}

function renderDeptSelectors(departments) {
  deptNames = departments.map((d) => d.name);
  const options = departments.map((d, i) => `<option value="${i}">${d.name}</option>`).join("");
  el.reqDept.innerHTML = options;
  el.relDept.innerHTML = options;
}

function buildVectorInputs(container, idPrefix) {
  container.innerHTML = RESOURCE_LABELS.map(
    (label, i) => `
      <div class="vector-cell">
        <label for="${idPrefix}${i}">${label}</label>
        <input id="${idPrefix}${i}" type="number" min="0" value="0" />
      </div>`
  ).join("");
}

function readVector(idPrefix) {
  return RESOURCE_LABELS.map((_, i) => {
    const input = document.getElementById(`${idPrefix}${i}`);
    return Math.max(0, parseInt(input.value, 10) || 0);
  });
}

function escapeHtml(str) {
  return str.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function applyState(state) {
  renderStatus(state.safe);
  renderResources(state.resources);
  renderLedger(state.departments);
  renderLog(state.log);
  renderDeptSelectors(state.departments);
}

function setFeedback(message, tone = "neutral") {
  el.consoleFeedback.textContent = message;
  el.consoleFeedback.style.color =
    tone === "good" ? "var(--safe)" : tone === "bad" ? "var(--deny)" : "var(--text-muted)";
}

/* ---------------------------------------------------------------------- */
/* API calls                                                              */
/* ---------------------------------------------------------------------- */

async function api(path, options) {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  if (!res.ok) throw new Error(`API error ${res.status}`);
  return res.json();
}

async function loadState() {
  try {
    const state = await api("/api/state");
    applyState(state);
    el.connState.textContent = "connected";
  } catch (err) {
    el.connState.textContent = "backend unreachable — start the server (npm start in /backend)";
  }
}

/* ---------------------------------------------------------------------- */
/* Event wiring                                                           */
/* ---------------------------------------------------------------------- */

buildVectorInputs(el.reqVector, "req");
buildVectorInputs(el.relVector, "rel");

el.requestForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const deptIndex = parseInt(el.reqDept.value, 10);
  const request = readVector("req");
  try {
    const result = await api("/api/request", {
      method: "POST",
      body: JSON.stringify({ deptIndex, request }),
    });
    setFeedback(result.message, result.status === "GRANTED" ? "good" : "bad");
    applyState(result.state);
  } catch {
    setFeedback("Request failed — is the backend running?", "bad");
  }
});

el.releaseForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const deptIndex = parseInt(el.relDept.value, 10);
  const release = readVector("rel");
  try {
    const result = await api("/api/release", {
      method: "POST",
      body: JSON.stringify({ deptIndex, release }),
    });
    setFeedback(result.message, "neutral");
    applyState(result.state);
  } catch {
    setFeedback("Release failed — is the backend running?", "bad");
  }
});

el.btnSchedule.addEventListener("click", async () => {
  try {
    const result = await api("/api/schedule-demo", { method: "POST" });
    setFeedback(`Scheduler dispatched: ${result.dispatchOrder.join(" → ")}`, "neutral");
    applyState(result.state);
  } catch {
    setFeedback("Scheduler run failed — is the backend running?", "bad");
  }
});

el.btnSafety.addEventListener("click", async () => {
  try {
    const result = await api("/api/safety");
    setFeedback(
      result.safe
        ? `Safe sequence: ${result.sequence.join(" → ")}`
        : "No safe sequence exists — system is unsafe.",
      result.safe ? "good" : "bad"
    );
    renderStatus(result.safe);
  } catch {
    setFeedback("Safety check failed — is the backend running?", "bad");
  }
});

el.btnDeadlock.addEventListener("click", async () => {
  try {
    const result = await api("/api/deadlock");
    setFeedback(
      result.deadlock ? `Deadlock risk: ${result.stuck.join(", ")}` : "No deadlock detected.",
      result.deadlock ? "bad" : "good"
    );
  } catch {
    setFeedback("Deadlock sweep failed — is the backend running?", "bad");
  }
});

el.btnReset.addEventListener("click", async () => {
  try {
    const result = await api("/api/reset", { method: "POST" });
    setFeedback(result.message, "neutral");
    applyState(result.state);
  } catch {
    setFeedback("Reset failed — is the backend running?", "bad");
  }
});

/* ---------------------------------------------------------------------- */
/* Boot + polling                                                         */
/* ---------------------------------------------------------------------- */

loadState();
setInterval(loadState, 5000);
