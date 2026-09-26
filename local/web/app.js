const $ = id => document.getElementById(id);
const canvas = $('canvas');
let engine, api, loaded = false, loading = true, duration = 0;
let dragging = false, resumeAfterDrag = false, pendingSeek = null, seekFrame = 0;
let closed = false, requestNumber = 0;
const fmt = s => {
  s = Math.max(0, Number.isFinite(s) ? s : 0);
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${(s % 60).toFixed(1).padStart(4, '0')}`;
};
function message(text) { $('status').textContent = text; }
function error(text = '') { $('error').textContent = text; $('error').hidden = !text; }
function controls() {
  $('open').disabled = loading || closed;
  for (const id of ['play', 'restart', 'seek']) $(id).disabled = loading || !loaded || closed;
}
function busy(text) { $('busytext').textContent = text; $('busy').hidden = false; }
function size() { const r = $('viewer').getBoundingClientRect(); return [Math.max(1, Math.floor(r.width)), Math.max(1, Math.floor(r.height))]; }
function paintTransport() {
  if (!loaded || !api || closed) return;
  const position = dragging ? Number($('seek').value) : api.position();
  if (!dragging) $('seek').value = Math.min(duration, position);
  $('time').textContent = `${fmt(position)} / ${fmt(duration)}`;
  const playing = !!api.playing();
  $('play').textContent = playing ? '❚❚' : '▶';
  $('play').setAttribute('aria-label', playing ? '暫停' : '播放');
  $('seek').setAttribute('aria-valuetext', `${fmt(position)}，共 ${fmt(duration)}`);
}
function seek(seconds) {
  api.seek(Math.max(0, Math.min(duration, seconds)));
  paintTransport();
}
function beginDrag() {
  if (!loaded || loading || dragging) return;
  dragging = true;
  resumeAfterDrag = !!api.playing();
  api.play(0);
}
function finishDrag() {
  if (!dragging) return;
  if (seekFrame) cancelAnimationFrame(seekFrame);
  seekFrame = 0; pendingSeek = null;
  api.seek(Number($('seek').value));
  dragging = false;
  api.play(resumeAfterDrag ? 1 : 0);
  paintTransport();
}
$('seek').addEventListener('pointerdown', beginDrag);
$('seek').addEventListener('keydown', e => { if (['ArrowLeft','ArrowRight','Home','End','PageUp','PageDown'].includes(e.key)) beginDrag(); });
$('seek').addEventListener('input', () => {
  beginDrag(); pendingSeek = Number($('seek').value);
  if (!seekFrame) seekFrame = requestAnimationFrame(() => {
    seekFrame = 0;
    if (pendingSeek !== null && loaded && !loading) api.seek(pendingSeek);
    pendingSeek = null; paintTransport();
  });
  paintTransport();
});
$('seek').addEventListener('change', finishDrag);
window.addEventListener('pointerup', finishDrag);
window.addEventListener('pointercancel', finishDrag);
$('seek').addEventListener('blur', finishDrag);
$('play').addEventListener('click', () => {
  if (api.position() >= duration - .02) seek(0);
  api.play(api.playing() ? 0 : 1); paintTransport();
});
$('restart').addEventListener('click', () => { api.play(0); seek(0); });
$('open').addEventListener('click', () => $('file').click());
$('file').addEventListener('change', () => {
  const file = $('file').files[0]; $('file').value = '';
  if (file) loadFile(file);
});
async function loadFile(file) {
  if (loading || closed) return;
  if (!/\.ulg$/i.test(file.name)) { error('請選擇 .ulg 格式的 ULog 檔案。'); return; }
  if (file.size > 100 * 1024 * 1024) { error('第一版支援單個 100 MiB 以下的 ULog，請選擇較小的檔案。'); return; }
  if (file.size < 16) { error('檔案過短，無法讀取 ULog。'); return; }
  finishDrag();
  const wasPlaying = loaded && !!api.playing();
  api.play(0); loading = true; controls(); error();
  busy('正在讀取飛行紀錄…'); message('載入中');
  const started = performance.now(); let pointer = 0;
  const ticket = ++requestNumber;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (ticket !== requestNumber || closed) return;
    if (![85,76,111,103,1,18,53].every((n,i) => bytes[i] === n)) throw new Error('ULog 檔頭不正確，請選擇有效的飛行紀錄。');
    // Let the browser paint the loading message before synchronous extraction.
    await new Promise(resolve => requestAnimationFrame(() => setTimeout(resolve, 0)));
    if (closed) return;
    pointer = engine._malloc(bytes.length);
    if (!pointer) throw new Error('記憶體不足，無法載入檔案。');
    engine.HEAPU8.set(bytes, pointer);
    const result = api.load(pointer, bytes.length);
    if (result !== 0) throw new Error('無法解析這份 ULog，或檔案缺少可回放的姿態資料。');
    duration = api.duration(); loaded = true;
    api.play(0); api.seek(0);
    $('seek').max = duration; $('seek').value = 0;
    $('filename').textContent = file.name;
    $('filename').title = file.name;
    const elapsed = (performance.now() - started) / 1000;
    $('filemeta').textContent = `${(file.size / 1048576).toFixed(1)} MiB · ${fmt(duration)} · 載入 ${elapsed.toFixed(2)} 秒`;
    $('welcome').hidden = true;
    message('就緒'); canvas.focus();
  } catch (e) {
    error(e.message || '載入失敗，請重新選擇檔案。');
    if (loaded) api.play(wasPlaying ? 1 : 0);
    message(loaded ? '保留原回放' : '等待選檔');
  } finally {
    if (pointer) engine._free(pointer);
    loading = false; $('busy').hidden = true; controls(); paintTransport();
  }
}
let dragDepth = 0;
window.addEventListener('dragenter', e => { if (e.dataTransfer?.types.includes('Files')) { e.preventDefault(); dragDepth++; if (!loading) $('drop-hint').hidden = false; } });
window.addEventListener('dragover', e => e.preventDefault());
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('drop-hint').hidden = true; } });
window.addEventListener('drop', e => {
  e.preventDefault(); dragDepth = 0; $('drop-hint').hidden = true;
  if (e.dataTransfer.files.length !== 1) { error('第一版一次開啟一個 ULog 檔案。'); return; }
  loadFile(e.dataTransfer.files[0]);
});
// Keep UI shortcuts from also changing the Hawkeye camera/playback.
window.addEventListener('keydown', e => {
  if (e.target !== canvas) { e.stopImmediatePropagation(); return; }
  if (e.code === 'Space') {
    // Handle this in the host UI: very short key presses may fall between
    // Raylib's frame-polled key transitions on WebKit. Do not toggle twice.
    e.preventDefault(); e.stopImmediatePropagation();
    if (loaded && !loading && !closed && !e.repeat) {
      if (api.position() >= duration - .02) seek(0);
      api.play(api.playing() ? 0 : 1); paintTransport();
    }
    return;
  }
  if (['ArrowLeft','ArrowRight'].includes(e.code)) e.preventDefault();
}, true);
window.addEventListener('blur', finishDrag);
new ResizeObserver(() => { if (api && !closed) api.resize(...size()); }).observe($('viewer'));
$('quit').addEventListener('click', async () => {
  try {
    const response = await fetch('api/shutdown', {method:'POST'});
    if (!response.ok) throw new Error('無法結束服務');
    closed = true; requestNumber++; engine?.ccall('hawkeye_destroy', null, [], []);
    controls(); $('quit').disabled = true; message('服務已結束');
    $('welcome').hidden = false;
    $('welcome').replaceChildren(Object.assign(document.createElement('h2'), {textContent:'服務已結束'}), Object.assign(document.createElement('p'), {textContent:'可以關閉此分頁；再次雙擊 App 即可重新開啟。'}));
  } catch (e) { error(e.message); }
});
setInterval(() => { if (!closed) fetch('api/health', {cache:'no-store'}).catch(() => {}); }, 15000);
setInterval(paintTransport, 100);
busy('正在啟動回放核心…');
try {
  // Keep boot errors visible even if a browser extension blocks an asset.
  const {default:createHawkeye} = await import('./engine/replay-core.js');
  engine = await createHawkeye({
    canvas, keyboardListeningElement:canvas,
    locateFile: name => new URL(`engine/${name.replace(/^hawkeye\./, 'replay-core.')}`, location.href).href,
    print: text => console.debug(text), printErr: text => console.warn(text),
    onAbort: reason => { error(`回放核心已停止：${reason}。請重新開啟頁面。`); closed = true; controls(); }
  });
  api = {
    load:engine.cwrap('hawkeye_load_ulog_bytes','number',['number','number']),
    play:engine.cwrap('hawkeye_set_playing',null,['number']),
    playing:engine.cwrap('hawkeye_get_playing','number',[]),
    seek:engine.cwrap('hawkeye_seek',null,['number']),
    duration:engine.cwrap('hawkeye_get_duration','number',[]),
    position:engine.cwrap('hawkeye_get_position','number',[]),
    resize:engine.cwrap('hawkeye_resize',null,['number','number'])
  };
  if (engine.ccall('hawkeye_init','number',['string','number','number'],['#canvas',...size()]) !== 0) throw new Error('無法啟動 WebGL 2，請確認瀏覽器已啟用硬體加速。');
  document.title = 'Flight Replay Local';
  loading = false; $('busy').hidden = true; message('等待選檔'); controls();
} catch (e) { $('busy').hidden = true; error(`啟動失敗：${e.message}`); message('啟動失敗'); }
