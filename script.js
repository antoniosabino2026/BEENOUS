/*
  BEENOUS — Better Energy Efficiency
  Versão 0.1.0 — simulação local.
  Este ficheiro não comunica com o ESP32 nem com hardware real.
*/

const LOAD_CONFIG = {
  high:   { name: "Carga 01", priority: "alta", watts: 120 },
  medium: { name: "Carga 02", priority: "média", watts: 180 },
  low:    { name: "Carga 03", priority: "baixa", watts: 250 }
};

const INITIAL_STATE = {
  high: true,
  medium: true,
  low: false
};

const state = { ...INITIAL_STATE };
const eventsList = document.querySelector("#eventsList");
const toast = document.querySelector("#toast");
let toastTimer;

function getTimeLabel() {
  return new Intl.DateTimeFormat("pt-PT", {
    hour: "2-digit",
    minute: "2-digit"
  }).format(new Date());
}

function showToast(message) {
  toast.textContent = message;
  toast.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("show"), 2600);
}

function addEvent(title, description, isOn = true) {
  const empty = eventsList.querySelector(".empty-events");
  if (empty) empty.remove();

  const item = document.createElement("div");
  item.className = "event-item";
  const mark = document.createElement("span");
  mark.className = `event-mark ${isOn ? "success" : "off"}`;
  mark.textContent = isOn ? "✓" : "–";

  const content = document.createElement("div");
  const heading = document.createElement("strong");
  heading.textContent = title;
  const detail = document.createElement("p");
  detail.textContent = description;
  content.append(heading, detail);

  const time = document.createElement("time");
  time.textContent = getTimeLabel();
  item.append(mark, content, time);
  eventsList.prepend(item);

  // Mantém o histórico curto para a interface não crescer indefinidamente.
  while (eventsList.children.length > 8) {
    eventsList.lastElementChild.remove();
  }
}

function calculateSimulatedPower() {
  return Object.keys(state).reduce((total, key) => {
    return total + (state[key] ? LOAD_CONFIG[key].watts : 0);
  }, 0);
}

function updateEnergyIndicators() {
  const activeCount = Object.values(state).filter(Boolean).length;
  const power = calculateSimulatedPower();
  const voltage = 230;

  // Corrente aproximada apenas para demonstração: I = P / V.
  const current = power / voltage;

  document.querySelector("#voltageValue").textContent = voltage.toFixed(0);
  document.querySelector("#currentValue").textContent =
    current.toLocaleString("pt-PT", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  document.querySelector("#powerValue").textContent =
    power.toLocaleString("pt-PT");
  document.querySelector("#activeLoadCount").textContent = `${activeCount}/3`;
  document.querySelector("#loadSummary").textContent =
    `${activeCount} de 3 cargas ${activeCount === 1 ? "ligada" : "ligadas"}`;

  const percentage = (activeCount / 3) * 100;
  document.querySelector(".donut").style.background =
    `conic-gradient(var(--green) 0 ${percentage}%, #253248 ${percentage}% 100%)`;
}

function renderLoad(key) {
  const card = document.querySelector(`[data-load-card="${key}"]`);
  const button = card.querySelector("[data-load]");
  const label = card.querySelector("[data-state-label]");
  const buttonLabel = card.querySelector("[data-button-label]");
  const isOn = state[key];

  card.classList.toggle("is-on", isOn);
  button.setAttribute("aria-pressed", String(isOn));
  label.textContent = isOn ? "Ligada" : "Desligada";
  buttonLabel.textContent = isOn ? "Desligar carga" : "Ligar carga";
}

function toggleLoad(key) {
  if (!(key in state)) return;

  state[key] = !state[key];
  renderLoad(key);
  updateEnergyIndicators();

  const config = LOAD_CONFIG[key];
  const action = state[key] ? "ligada" : "desligada";
  addEvent(
    `${config.name} ${action}`,
    `Prioridade ${config.priority} · simulação local.`,
    state[key]
  );
  showToast(`${config.name}: ${action}. Apenas simulação local.`);
}

function resetSimulation() {
  Object.assign(state, INITIAL_STATE);
  Object.keys(state).forEach(renderLoad);
  updateEnergyIndicators();
  eventsList.replaceChildren();
  addEvent("Simulação reposta", "Estados demonstrativos restaurados.");
  addEvent("Carga 01 ligada", "Prioridade alta · simulação local.");
  addEvent("Carga 02 ligada", "Prioridade média · simulação local.");
  showToast("Simulação reposta para os valores iniciais.");
}

document.querySelectorAll("[data-load]").forEach(button => {
  button.addEventListener("click", () => toggleLoad(button.dataset.load));
});

document.querySelector("#resetButton").addEventListener("click", resetSimulation);

document.querySelector("#clearEvents").addEventListener("click", () => {
  eventsList.replaceChildren();
  const empty = document.createElement("div");
  empty.className = "empty-events";
  empty.textContent = "Ainda não há eventos registados.";
  eventsList.append(empty);
  showToast("Histórico visual limpo.");
});

const sidebar = document.querySelector("#sidebar");
document.querySelector("#menuToggle").addEventListener("click", () => {
  sidebar.classList.toggle("open");
});

document.querySelectorAll(".nav-link").forEach(link => {
  link.addEventListener("click", () => {
    document.querySelectorAll(".nav-link").forEach(item => item.classList.remove("active"));
    link.classList.add("active");
    sidebar.classList.remove("open");
  });
});

// Renderização inicial a partir do estado demonstrativo.
Object.keys(state).forEach(renderLoad);
updateEnergyIndicators();
