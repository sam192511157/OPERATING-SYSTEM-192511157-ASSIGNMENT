/**
 * ============================================================================
 * UniCore Backend — OS Resource Orchestration System for a Smart University
 * ----------------------------------------------------------------------------
 * Express REST API that ports the same OS logic used in the C simulation
 * (unicore.c) to JavaScript:
 *   - A shared resource pool (Classrooms, Lab-PCs, Printers, Bandwidth,
 *     Parking) allocated across competing "department" processes.
 *   - Deadlock AVOIDANCE via the Banker's Algorithm (safety algorithm +
 *     resource-request algorithm).
 *   - A priority-ordered scheduler for batches of pending requests.
 *   - Deadlock DETECTION as a reporting fallback.
 *   - An in-memory orchestration event log.
 *
 * Run:
 *   npm install
 *   npm start
 *   -> API listens on http://localhost:4000
 * ============================================================================
 */

const express = require("express");
const cors = require("cors");

const app = express();
app.use(cors());
app.use(express.json());

const PORT = process.env.PORT || 4000;

/* ---------------------------------------------------------------------- */
/* Resource + department model                                            */
/* ---------------------------------------------------------------------- */

const RESOURCES = ["Classrooms", "Lab-PCs", "Printers", "Bandwidth (Gbps)", "Parking-Slots"];
const NUM_RESOURCES = RESOURCES.length;

const SEED_TOTAL = [10, 40, 6, 100, 50];

const SEED_DEPARTMENTS = [
  { name: "CSE-Dept",     priority: 2, max: [3, 20, 2, 40, 10] },
  { name: "ECE-Dept",     priority: 3, max: [2, 10, 1, 20, 8] },
  { name: "Mechanical",   priority: 5, max: [2, 5, 1, 10, 12] },
  { name: "Admin-Office", priority: 1, max: [1, 2, 2, 5, 4] },
  { name: "Library",      priority: 4, max: [2, 8, 1, 15, 6] },
  { name: "Exam-Cell",    priority: 1, max: [2, 4, 1, 10, 3] },
];

let state = null; // populated by resetState()

function resetState() {
  state = {
    total: [...SEED_TOTAL],
    available: [...SEED_TOTAL],
    departments: SEED_DEPARTMENTS.map((d) => ({
      name: d.name,
      priority: d.priority,
      max: [...d.max],
      allocation: new Array(NUM_RESOURCES).fill(0),
      need: [...d.max],
      blocked: false,
    })),
    log: [],
  };
  logEvent(`System initialised: ${state.departments.length} departments, ${NUM_RESOURCES} resource types.`);
}

function logEvent(message) {
  const stamp = new Date().toISOString().substring(11, 19); // HH:MM:SS
  state.log.push({ time: stamp, message });
  if (state.log.length > 300) state.log.shift();
}

function recomputeNeed(dept) {
  dept.need = dept.max.map((m, i) => m - dept.allocation[i]);
}

/* ---------------------------------------------------------------------- */
/* Banker's Algorithm — Safety Algorithm                                  */
/* ---------------------------------------------------------------------- */

function isSafeState() {
  const work = [...state.available];
  const finish = new Array(state.departments.length).fill(false);
  const sequence = [];
  let count = 0;

  while (count < state.departments.length) {
    let progressed = false;

    for (let i = 0; i < state.departments.length; i++) {
      if (finish[i]) continue;
      const d = state.departments[i];
      const canRun = d.need.every((n, r) => n <= work[r]);

      if (canRun) {
        for (let r = 0; r < NUM_RESOURCES; r++) work[r] += d.allocation[r];
        finish[i] = true;
        sequence.push(i);
        count++;
        progressed = true;
      }
    }

    if (!progressed) break;
  }

  return { safe: count === state.departments.length, sequence };
}

/* ---------------------------------------------------------------------- */
/* Banker's Algorithm — Resource-Request Algorithm                        */
/* ---------------------------------------------------------------------- */

function requestResources(deptIndex, req) {
  const d = state.departments[deptIndex];
  if (!d) return { status: "INVALID", message: "Unknown department." };

  for (let r = 0; r < NUM_RESOURCES; r++) {
    if (req[r] > d.need[r]) {
      const msg = `DENY ${d.name} — request exceeds declared max need for ${RESOURCES[r]}.`;
      logEvent(msg);
      return { status: "DENIED_EXCEEDS_NEED", message: msg };
    }
  }

  for (let r = 0; r < NUM_RESOURCES; r++) {
    if (req[r] > state.available[r]) {
      d.blocked = true;
      const msg = `WAIT ${d.name} blocked — insufficient ${RESOURCES[r]} (need ${req[r]}, available ${state.available[r]}).`;
      logEvent(msg);
      return { status: "DENIED_NOT_AVAILABLE", message: msg };
    }
  }

  // Tentatively allocate, then test safety
  const saveAvail = [...state.available];
  const saveAlloc = [...d.allocation];
  const saveNeed = [...d.need];

  for (let r = 0; r < NUM_RESOURCES; r++) {
    state.available[r] -= req[r];
    d.allocation[r] += req[r];
  }
  recomputeNeed(d);

  const { safe, sequence } = isSafeState();

  if (safe) {
    d.blocked = false;
    const msg = `GRANT ${d.name} — allocated request; system remains SAFE.`;
    logEvent(msg);
    return { status: "GRANTED", message: msg, safeSequence: sequence.map((i) => state.departments[i].name) };
  } else {
    state.available = saveAvail;
    d.allocation = saveAlloc;
    d.need = saveNeed;
    d.blocked = true;
    const msg = `DENY ${d.name} — request refused: would move system to UNSAFE state.`;
    logEvent(msg);
    return { status: "DENIED_UNSAFE", message: msg };
  }
}

function releaseResources(deptIndex, rel) {
  const d = state.departments[deptIndex];
  if (!d) return { status: "INVALID", message: "Unknown department." };

  for (let r = 0; r < NUM_RESOURCES; r++) {
    const amt = Math.min(rel[r] || 0, d.allocation[r]);
    d.allocation[r] -= amt;
    state.available[r] += amt;
  }
  recomputeNeed(d);
  d.blocked = false;

  const msg = `RELEASE ${d.name} — returned resources to the shared pool.`;
  logEvent(msg);
  return { status: "RELEASED", message: msg };
}

/* ---------------------------------------------------------------------- */
/* Deadlock detection (fallback / reporting)                              */
/* ---------------------------------------------------------------------- */

function detectDeadlock() {
  const { safe, sequence } = isSafeState();
  if (safe) return { deadlock: false, stuck: [] };

  const inSeq = new Set(sequence);
  const stuck = state.departments
    .map((d, i) => i)
    .filter((i) => !inSeq.has(i))
    .map((i) => state.departments[i].name);

  return { deadlock: true, stuck };
}

/* ---------------------------------------------------------------------- */
/* Priority scheduler for a batch of pending requests                     */
/* ---------------------------------------------------------------------- */

function scheduleAndProcess(pending) {
  // Stable sort by department priority (ascending = more urgent), preserving
  // arrival order for ties (round-robin behaviour among equal priorities).
  const order = pending
    .map((p, idx) => ({ ...p, idx }))
    .sort((a, b) => {
      const pa = state.departments[a.deptIndex]?.priority ?? 999;
      const pb = state.departments[b.deptIndex]?.priority ?? 999;
      if (pa !== pb) return pa - pb;
      return a.idx - b.idx;
    });

  const dispatchOrder = order.map((p) => state.departments[p.deptIndex]?.name || "?");
  const results = order.map((p) => ({
    department: state.departments[p.deptIndex]?.name || "?",
    result: requestResources(p.deptIndex, p.request),
  }));

  return { dispatchOrder, results };
}

/* ---------------------------------------------------------------------- */
/* Serialisation helper                                                   */
/* ---------------------------------------------------------------------- */

function serializeState() {
  const { safe } = isSafeState();
  return {
    resources: RESOURCES.map((name, i) => ({
      name,
      total: state.total[i],
      available: state.available[i],
      inUse: state.total[i] - state.available[i],
    })),
    departments: state.departments.map((d) => ({
      name: d.name,
      priority: d.priority,
      allocation: d.allocation,
      max: d.max,
      need: d.need,
      blocked: d.blocked,
    })),
    safe,
    log: state.log.slice(-50).reverse(),
  };
}

/* ---------------------------------------------------------------------- */
/* Routes                                                                  */
/* ---------------------------------------------------------------------- */

app.get("/api/health", (_req, res) => res.json({ ok: true, service: "unicore-backend" }));

app.get("/api/state", (_req, res) => res.json(serializeState()));

app.get("/api/safety", (_req, res) => {
  const { safe, sequence } = isSafeState();
  res.json({ safe, sequence: sequence.map((i) => state.departments[i].name) });
});

app.get("/api/deadlock", (_req, res) => res.json(detectDeadlock()));

app.get("/api/log", (_req, res) => res.json({ log: state.log.slice().reverse() }));

app.post("/api/request", (req, res) => {
  const { deptIndex, request } = req.body || {};
  if (
    typeof deptIndex !== "number" ||
    !Array.isArray(request) ||
    request.length !== NUM_RESOURCES ||
    request.some((v) => typeof v !== "number" || v < 0)
  ) {
    return res.status(400).json({ status: "INVALID", message: "Malformed request payload." });
  }
  const result = requestResources(deptIndex, request);
  res.json({ ...result, state: serializeState() });
});

app.post("/api/release", (req, res) => {
  const { deptIndex, release } = req.body || {};
  if (
    typeof deptIndex !== "number" ||
    !Array.isArray(release) ||
    release.length !== NUM_RESOURCES ||
    release.some((v) => typeof v !== "number" || v < 0)
  ) {
    return res.status(400).json({ status: "INVALID", message: "Malformed release payload." });
  }
  const result = releaseResources(deptIndex, release);
  res.json({ ...result, state: serializeState() });
});

app.post("/api/schedule-demo", (_req, res) => {
  // Dispatches a representative batch of simultaneous requests, exactly
  // like the C program's demo scenario, through the priority scheduler.
  const batch = [
    { deptIndex: 0, request: [1, 8, 1, 15, 2] },
    { deptIndex: 1, request: [1, 4, 0, 8, 3] },
    { deptIndex: 2, request: [1, 2, 1, 4, 5] },
    { deptIndex: 3, request: [1, 1, 1, 2, 2] },
    { deptIndex: 4, request: [1, 3, 0, 5, 1] },
    { deptIndex: 5, request: [1, 2, 1, 4, 1] },
  ];
  const outcome = scheduleAndProcess(batch);
  res.json({ ...outcome, state: serializeState() });
});

app.post("/api/reset", (_req, res) => {
  resetState();
  res.json({ message: "System reset to initial seed state.", state: serializeState() });
});

app.use((req, res) => res.status(404).json({ status: "NOT_FOUND", path: req.path }));

/* ---------------------------------------------------------------------- */
/* Boot                                                                    */
/* ---------------------------------------------------------------------- */

resetState();

app.listen(PORT, () => {
  console.log(`UniCore backend listening on http://localhost:${PORT}`);
  console.log(`Departments: ${RESOURCES.length} resource types, ${SEED_DEPARTMENTS.length} departments seeded.`);
});
