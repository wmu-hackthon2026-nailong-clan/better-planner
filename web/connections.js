'use strict';
const $ = id => document.getElementById(id);
let state = null, refreshing = false;
const busy = new Set();
function render() {
  const missing = new Set((state.deps.missing || []).map(x => x.platform));
  $('notice').textContent = missing.size ? '连接器未就绪：' + [...missing].join('、') + '。请先运行项目中的 scripts/setup_desktop.py。' : state.running ? '正在同步，请稍候。完成后桌面日历会自动更新。' : '连接环境已就绪。先登录，再选择要同步的平台；已有登录态可直接同步。';
  for (const [provider, label] of [['chaoxing', '学习通'], ['zhihuishu', '智慧树']]) {
    const job = (state.auth || {})[provider];
    const pending = job && ['running', 'qr'].includes(job.status);
    if (job) $(provider + '-status').textContent = job.message;
    const login = provider === 'chaoxing' ? $('chaoxing-login').querySelector('button') : $('zhihuishu-login');
    login.disabled = missing.has(label) || pending || busy.has(provider);
    $(provider + '-sync').disabled = missing.has(label) || pending || state.running || busy.has(provider);
    $(provider + '-sync').textContent = state.running && (!state.active_provider || state.active_provider === provider) ? '同步进行中…' : '同步' + label;
    const rows = state.sync.filter(r => r.stream.startsWith(label));
    if (!busy.has(provider) && rows.length) {
      const pendingSync = state.running && (!state.active_provider || state.active_provider === provider);
      const failures = rows.filter(r => r.error).length;
      $(provider + '-result').textContent = pendingSync ? '正在读取平台数据，下方显示各项最近结果…' : failures ? '有 ' + failures + ' 项未完整成功，请查看下方原因和已保存明细。' : '读取完成，请在下方核对课程和日期（不是完整课程清单核验）。';
    }
  }
  const qr = state.auth.zhihuishu;
  const hasQR = qr && qr.status === 'qr' && typeof qr.image === 'string';
  $('qr-image').hidden = !hasQR;
  $('qr-hint').hidden = !!hasQR;
  if (hasQR) $('qr-image').src = 'data:image/png;base64,' + qr.image;
  else $('qr-image').removeAttribute('src');
  if (qr && qr.status === 'success') $('qr-hint').textContent = '已登录，可以同步任务';
  else $('qr-hint').textContent = '点击下方按钮获取二维码，在知到 App 中确认登录';
  renderReports();
  renderInventory();
  const undated = state.events.filter(row => row.kind !== 'class' && !row.end);
  $('undated-section').hidden = !undated.length;
  $('undated').replaceChildren();
  for (const row of undated) {
    const line = document.createElement('p');
    line.textContent = row.title + (row.course ? ' · ' + row.course : '') + (row.completed ? ' · 已完成' : '');
    $('undated').append(line);
  }
}
const kindNames = {class: '课表', homework: '作业', exam: '考试', other: '任务'};
const changeNames = {added: '新增', updated: '更新', unchanged: '未变化'};
let reportKey = '', inventoryKey = '';
function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}
function dateText(value) {
  if (!value) return '未提供日期';
  return new Intl.DateTimeFormat('zh-CN', {timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false}).format(new Date(value));
}
function eligible(row) { return !!row.end && (row.kind !== 'class' || !!row.start); }
function recordTable(rows, withChange) {
  const wrap = element('div', undefined, 'table-scroll');
  const table = element('table');
  const head = element('tr');
  for (const title of ['课程 / 标题', '类型', '日历时间（北京时间）', '地点 / 教师', withChange ? '本次处理' : '来源', '日历状态']) head.append(element('th', title));
  const thead = element('thead'); thead.append(head); table.append(thead);
  const body = element('tbody');
  for (const row of rows) {
    const tr = element('tr'), title = element('td');
    title.append(element('strong', row.course || row.title));
    if (row.course && row.title !== row.course) title.append(element('div', row.title));
    const time = row.kind === 'class' ? dateText(row.start) + ' — ' + dateText(row.end) : (row.end ? '截止 ' + dateText(row.end) : '未提供截止时间');
    tr.append(title, element('td', kindNames[row.kind] || row.kind), element('td', time), element('td', [row.location, row.teacher].filter(Boolean).join(' · ') || '未提供'), element('td', withChange ? changeNames[row.change] : (row.source === 'chaoxing' ? '学习通' : '智慧树')), element('td', eligible(row) ? '可进入日历' : '暂不进入：缺少日期', eligible(row) ? 'success' : 'warning'));
    body.append(tr);
  }
  table.append(body); wrap.append(table); return wrap;
}
function renderReports() {
  const key = JSON.stringify(state.sync);
  if (key === reportKey) return;
  reportKey = key;
  const root = $('history'); root.replaceChildren();
  if (!state.sync.length) root.textContent = '尚未同步。选择平台并点击同步后，这里会显示结果。';
  for (const row of state.sync) {
    const section = element('div', undefined, 'sync-report');
    section.append(element('h3', row.stream), element('p', '最近尝试：' + dateText(row.attempted_at), 'report-time'));
    const report = row.report;
    if (!report) {
      section.append(element('p', row.error ? '读取失败：' + row.error + '。历史数据保留。' : '这是一条旧版同步记录，没有本次明细。请重新同步生成核对结果。', row.error ? 'error' : 'warning'));
      root.append(section); continue;
    }
    const items = report.items || [], dated = items.filter(eligible);
    const courses = new Set(items.map(r => r.course).filter(Boolean));
    const counts = element('div', undefined, 'report-counts');
    for (const text of [courses.size + ' 门有返回内容的课程', items.length + ' 条记录', '新增 ' + report.added, '更新 ' + report.updated, '未变化 ' + report.unchanged, '有日期 ' + dated.length, '缺少日期 ' + (items.length - dated.length)]) counts.append(element('span', text));
    section.append(element('p', report.status === 'partial' ? '部分成功：' + row.error : items.length ? '读取并保存完成' : '读取完成，本次返回 0 条', report.status === 'partial' ? 'warning' : 'success'), counts);
    if (report.received > items.length) section.append(element('p', '平台返回 ' + report.received + ' 条，按记录标识去重后为 ' + items.length + ' 条。'));
    if (!items.length) section.append(element('p', '未返回符合当前同步范围的记录；不代表账号没有课程。历史记录仍保留。'));
    if (row.stream === '智慧树任务') section.append(element('p', '范围：共享课作业/考试、AI 知识图谱发布任务/考试及课程进度；暂不含翻转课，不导入每周上课时间。进度仅展示，不作为日历事项。'));
    if (report.courses?.length) {
      const catalog = element('details');
      catalog.open = true;
      catalog.append(element('summary', '发现 ' + report.courses.length + ' 门课程 · 包括没有返回任务的课程'));
      for (const course of report.courses) {
        const line = element('div', undefined, 'log');
        line.append(element('strong', course.name + ' · ' + course.system));
        if (course.system === 'AI 知识图谱') {
          line.append(element('span', '任务列表：' + (course.tasks_status === 'success' ? '已读取' : '读取失败') + ' · 考试待办：' + (course.exams_status === 'success' ? '已读取' : '读取失败') + ' · 去重后 ' + (course.records || 0) + ' 条'));
          const p = course.progress;
          line.append(element('small', p ? '知识点 ' + (p.knowledgeCount ?? '未知') + ' 个 · 学习进度 ' + (p.studyProgress ?? '未知') + '% · 掌握度 ' + (p.masteryPercentage ?? '未知') + '%' : '知识点进度未读取，不能视为 0%'));
        }
        catalog.append(line);
      }
      section.append(catalog);
    }
    if (row.stream === '学习通课表') section.append(element('p', '范围：当前周与下周，不代表整学期课表。'));
    if (items.length) {
      const details = element('details');
      details.append(element('summary', '查看 ' + items.length + ' 条明细 · ' + [...courses].join('、')));
      details.append(recordTable(items, true)); section.append(details);
    }
    root.append(section);
  }
}
function renderInventory() {
  const provider = $('inventory-provider').value, query = $('inventory-search').value.trim().toLowerCase();
  const rows = state.events.filter(r => ['chaoxing','zhihuishu'].includes(r.source) && (!provider || r.source === provider) && (!query || (r.course + ' ' + r.title).toLowerCase().includes(query)));
  const key = JSON.stringify(rows);
  if (key === inventoryKey) return;
  inventoryKey = key;
  const dated = rows.filter(eligible).length;
  $('inventory-count').textContent = rows.length + ' 条已保存 · ' + dated + ' 条可进入日历 · ' + (rows.length - dated) + ' 条缺少日期';
  $('inventory').replaceChildren(rows.length ? recordTable(rows, false) : element('p', '没有匹配的已保存记录。'));
}
$('inventory-provider').addEventListener('change', () => { if (state) renderInventory(); });
$('inventory-search').addEventListener('input', () => { if (state) renderInventory(); });
async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try {
    const res = await fetch('/api/state', {cache: 'no-store'});
    const data = await res.json(); if (!res.ok) throw Error(data.error);
    state = data; render();
  } catch (error) {
    $('notice').textContent = '无法连接本地服务。请保持桌面应用打开，再点击“连接与同步”重试。';
  } finally { refreshing = false; }
}
async function post(path, body) {
  if (!state) throw Error('本地服务尚未就绪，请稍后重试');
  const res = await fetch(path, {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Planner-Token': state.token}, body: JSON.stringify(body)});
  const data = await res.json(); if (!res.ok) throw Error(data.error || '操作失败');
  return data;
}
$('chaoxing-login').addEventListener('submit', async event => {
  event.preventDefault(); const form = event.currentTarget;
  const body = {provider: 'chaoxing', phone: form.elements.phone.value, password: form.elements.password.value, remember: form.elements.remember.checked};
  busy.add('chaoxing'); if (state) render();
  try { await post('/api/auth/login', body); $('chaoxing-result').textContent = '登录请求已提交，请等待结果。'; }
  catch (error) { $('chaoxing-result').textContent = error.message; }
  finally { body.password = ''; form.elements.password.value = ''; busy.delete('chaoxing'); await refresh(); }
});
$('zhihuishu-login').addEventListener('click', async () => {
  busy.add('zhihuishu'); if (state) render();
  try { await post('/api/auth/login', {provider: 'zhihuishu'}); $('zhihuishu-result').textContent = '正在获取二维码…'; }
  catch (error) { $('zhihuishu-result').textContent = error.message; }
  finally { busy.delete('zhihuishu'); await refresh(); }
});
for (const provider of ['chaoxing', 'zhihuishu']) $(provider + '-sync').addEventListener('click', async () => {
  busy.add(provider); if (state) render();
  try { const result = await post('/api/sync', {provider}); $(provider + '-result').textContent = result.started ? '已开始同步，请在下方查看各项结果。' : '已有同步任务正在运行。'; }
  catch (error) { $(provider + '-result').textContent = error.message; }
  finally { busy.delete(provider); await refresh(); }
});
refresh(); setInterval(refresh, 1800);
