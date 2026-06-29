// 本地 AI 伴侶 — 前端邏輯（純 JS，無框架）

const $ = (sel) => document.querySelector(sel);

const portrait = $("#portrait");
const scene = $("#scene");
const bubble = $("#bubble");
const bubbleText = $("#bubble-text");
const bubbleName = $("#bubble-name");
const msgInput = $("#msg");
const form = $("#input-bar");
const sendBtn = $("#btn-send");
const statusBadge = $("#status-badge");

let companionName = "伴侶";

// ---- 聊天 ----
form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const message = msgInput.value.trim();
  if (!message) return;
  msgInput.value = "";
  sendBtn.disabled = true;
  showBubble("…", "（思考中）");

  try {
    const res = await fetch("/chat", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ message }),
    });
    if (!res.ok) throw new Error(await res.text());
    const data = await res.json();
    showBubble(data.reply, companionName);
    if (data.character_image) portrait.src = data.character_image;
    if (data.scene_image) {
      scene.style.backgroundImage = `url(${data.scene_image})`;
    }
  } catch (err) {
    showBubble("（連線出錯：" + err.message + "）", "系統");
  } finally {
    sendBtn.disabled = false;
    msgInput.focus();
  }
});

function showBubble(text, name) {
  bubbleText.textContent = text;
  bubbleName.textContent = name;
  bubble.classList.remove("hidden");
}

// ---- 狀態輪詢（睡眠/做夢）----
async function pollStatus() {
  try {
    const res = await fetch("/status");
    const s = await res.json();
    if (s.dreaming) {
      statusBadge.textContent = "💤 做夢中…" + (s.current_subject ? `（${s.current_subject}）` : "");
      statusBadge.classList.remove("hidden");
      portrait.style.filter = "drop-shadow(0 12px 30px rgba(0,0,0,.5)) brightness(0.6)";
    } else if (s.sleeping) {
      statusBadge.textContent = "😴 睡眠中";
      statusBadge.classList.remove("hidden");
      portrait.style.filter = "drop-shadow(0 12px 30px rgba(0,0,0,.5)) brightness(0.7)";
    } else {
      statusBadge.classList.add("hidden");
      portrait.style.filter = "drop-shadow(0 12px 30px rgba(0,0,0,.5))";
    }
  } catch (_) { /* 忽略 */ }
}
setInterval(pollStatus, 15000);
pollStatus();

// ---- 側邊面板 ----
const panel = $("#panel");
$("#btn-panel").addEventListener("click", () => { panel.classList.remove("hidden"); loadMemory(); });
$("#panel-close").addEventListener("click", () => panel.classList.add("hidden"));

document.querySelectorAll(".tab").forEach((tab) => {
  tab.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
    document.querySelectorAll(".tab-pane").forEach((p) => p.classList.remove("active"));
    tab.classList.add("active");
    const name = tab.dataset.tab;
    $("#tab-" + name).classList.add("active");
    if (name === "memory") loadMemory();
    if (name === "dreams") loadDreams();
    if (name === "skills") loadSkills();
  });
});

// ---- 記憶面板 ----
async function loadMemory() {
  const res = await fetch("/memory");
  const items = await res.json();
  const list = $("#memory-list");
  list.innerHTML = "";
  $("#memory-editor").classList.add("hidden");
  list.classList.remove("hidden");
  items.forEach((it) => {
    const div = document.createElement("div");
    div.className = "list-item";
    div.innerHTML = `<div class="cat">${it.category}</div><div class="name">${it.name}</div>`;
    div.addEventListener("click", () => openMemory(it.path));
    list.appendChild(div);
  });
  if (items.length === 0) list.innerHTML = "<p style='opacity:.6'>還沒有記憶。聊一聊就會慢慢累積。</p>";
}

async function openMemory(path) {
  const res = await fetch("/memory/" + path);
  const data = await res.json();
  $("#memory-list").classList.add("hidden");
  const editor = $("#memory-editor");
  editor.classList.remove("hidden");
  $("#editor-path").textContent = path;
  $("#editor-text").value = data.content;

  $("#editor-save").onclick = async () => {
    await fetch("/memory/" + path, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ content: $("#editor-text").value }),
    });
    loadMemory();
  };
  $("#editor-delete").onclick = async () => {
    if (!confirm("確定刪除這筆記憶？")) return;
    await fetch("/memory/" + path, { method: "DELETE" });
    loadMemory();
  };
  $("#editor-close").onclick = () => loadMemory();
}

// ---- 夢境畫廊 ----
async function loadDreams() {
  const res = await fetch("/dreams");
  const items = await res.json();
  const grid = $("#dreams-grid");
  grid.innerHTML = "";
  if (items.length === 0) {
    grid.innerHTML = "<p style='opacity:.6'>還沒有夢境。到睡覺時間它會開始畫。</p>";
    return;
  }
  items.forEach((d) => {
    const fig = document.createElement("figure");
    fig.innerHTML = `<img src="${d.image}" alt="${d.subject}"><figcaption>${d.subject}</figcaption>`;
    grid.appendChild(fig);
  });
}

// ---- Skills ----
async function loadSkills() {
  const res = await fetch("/skills");
  const items = await res.json();
  const list = $("#skills-list");
  list.innerHTML = "";
  items.forEach((s) => {
    const div = document.createElement("div");
    div.className = "list-item";
    div.innerHTML = `<div class="cat">${s.type} · ${s.triggers.join("、")}</div>
                     <div class="name">${s.name}</div>
                     <div style="font-size:13px;opacity:.7">${s.description}</div>`;
    list.appendChild(div);
  });
}

// 取得伴侶名字（從第一筆人物記憶推測）
fetch("/memory").then((r) => r.json()).then((items) => {
  const c = items.find((i) => i.path === "characters/companion.md");
  if (c) { companionName = c.name; }
});
