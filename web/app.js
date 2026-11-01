const $ = s => document.querySelector(s);
const names = {chaoxing:'学习通',zhihuishu:'智慧树',manual:'手动添加'};
const kinds = {homework:'作业',exam:'考试',class:'课程',other:'其他'};
let state = {events:[],sync:[]}, view = 'overview', offset = 0, loading = false;
const fmt = (v,options={}) => v ? new Intl.DateTimeFormat('zh-CN',{timeZone:'Asia/Shanghai',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',hour12:false,...options}).format(new Date(v)) : '截止时间未知';
const dayKey = date => new Intl.DateTimeFormat('en-CA',{timeZone:'Asia/Shanghai',year:'numeric',month:'2-digit',day:'2-digit'}).format(date);
const escape = s => String(s ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const empty = (title,note='') => `<div class="empty"><b>${escape(title)}</b>${escape(note)}</div>`;
const deadlineDelta = e => e.end ? new Date(e.end)-new Date(state.now) : Infinity;
function urgency(e) {const n=deadlineDelta(e); return n<0?'overdue':n<=72*3600000?'urgent':'';}
function countdown(e) {if(e.completed)return '已完成';if(!e.end)return '待确认截止时间';const n=deadlineDelta(e);if(n<0)return '已逾期';if(n<3600000)return `剩余 ${Math.ceil(n/60000)} 分钟`;if(n<86400000)return `剩余 ${Math.ceil(n/3600000)} 小时`;return `剩余 ${Math.ceil(n/86400000)} 天`;}
function tasks(){return state.events.filter(e=>e.kind!=='class');}
function taskHTML(e){return `<div class="task-row ${e.completed?'done':''}"><input type="checkbox" data-complete="${e.id}" ${e.completed?'checked':''} ${e.platform_done?'disabled':''} aria-label="标记 ${escape(e.title)} 完成"><div><h3>${escape(e.title)}</h3><p>${escape(e.course||'未分配课程')} · ${kinds[e.kind]}</p></div><span class="tag ${e.source}">${names[e.source]}</span><div class="deadline ${e.completed?'':urgency(e)}">${fmt(e.end)}<small>${countdown(e)}</small></div><div class="task-links">${e.url?`<a href="${escape(e.url)}" target="_blank" rel="noopener noreferrer">打开 ↗</a>`:''}${e.source==='manual'?`<button data-delete="${e.id}">删除</button>`:''}</div></div>`;}
function switchView(v){view=v;document.querySelectorAll('.view').forEach(el=>el.hidden=el.id!==v);document.querySelectorAll('.nav').forEach(el=>el.classList.toggle('active',el.dataset.view===v));$('#heading').textContent={overview:'学习总览',week:'周课表',tasks:'截止清单',connections:'数据连接'}[v];$('#stats').hidden=v==='connections';render();}
function render(){
 const pending=tasks().filter(e=>!e.completed), urgent=pending.filter(e=>deadlineDelta(e)>=0&&deadlineDelta(e)<=72*3600000), overdue=pending.filter(e=>deadlineDelta(e)<0);
 const today=dayKey(new Date(state.now||Date.now()));
 const todayClasses=state.events.filter(e=>e.kind==='class'&&e.start&&dayKey(new Date(e.start))===today);
 $('#today').textContent=fmt(state.now||new Date(),{year:'numeric',weekday:'long',hour:undefined,minute:undefined})+' / MY STUDY SPACE';
 $('#stats').innerHTML=[['今日课程',todayClasses.length,'以同步课表为准'],['待完成任务',pending.length,'两个平台与手动任务'],['即将截止',urgent.length,'未来 72 小时内'],['已逾期',overdue.length,'记得确认是否已经提交']].map(([label,n,sub],i)=>`<div class="stat ${i===2?'accent':''}"><p>${label}</p><strong>${n.toString().padStart(2,'0')}</strong><small>${sub}</small></div>`).join('');
 $('#urgent-count').innerHTML=`${urgent.length}<span>项在 72 小时内截止</span>`;
 const next=state.events.filter(e=>e.kind==='class'&&new Date(e.end)>=new Date(state.now)).sort((a,b)=>new Date(a.start)-new Date(b.start)).slice(0,3);
 $('#next-classes').innerHTML=next.length?next.map(e=>`<div class="class-row"><div class="class-time">${fmt(e.start,{month:undefined,day:undefined})}<small>${fmt(e.start,{hour:undefined,minute:undefined,weekday:'short'})}</small></div><div class="class-detail"><h3>${escape(e.title)}</h3><p>${escape([e.location,e.teacher].filter(Boolean).join(' · ')||'暂无教室信息')}</p></div><span class="class-mark">${names[e.source]}</span></div>`).join(''):empty('下一节课，还在路上','同步学习通课表，或添加你的第一节课。');
 $('#recent-tasks').innerHTML=pending.length?pending.slice(0,5).map(taskHTML).join(''):empty('暂时没有待办任务','还未连接平台？前往「数据连接」完成配置。');
 renderTasks();renderWeek();renderSync();renderAuth();renderSubscription();
const deps=state.deps||{};
if(!deps.ready){$('#sync').disabled=true;$('#sync').textContent='↻ 同步平台（连接器未就绪）';$('#sync-banner').hidden=false;$('#sync-banner').textContent='连接器未安装：'+deps.missing.map(m=>m.platform).join('、')+'。'+deps.hint;}
else{$('#sync').disabled=!!state.running;$('#sync').textContent=state.running?'正在同步…':'↻ 同步平台';const failures=state.sync.filter(x=>x.error);$('#sync-banner').hidden=!failures.length;$('#sync-banner').textContent=`${failures.length} 项同步未完整成功，当前显示已保存的数据。请在「数据连接」查看详情。`;}
}
function renderTasks(){const q=$('#search').value.toLowerCase(),source=$('#source').value,status=$('#status').value;const rows=tasks().filter(e=>(!q||(e.title+' '+e.course).toLowerCase().includes(q))&&(!source||e.source===source)&&(status==='all'||(status==='done'?e.completed:!e.completed&&(status==='pending'||(status==='overdue'?urgency(e)==='overdue':urgency(e)==='urgent')))));$('#all-tasks').innerHTML=rows.length?rows.map(taskHTML).join(''):empty('没有符合条件的任务','试试更换平台、状态或搜索关键词。');}
function renderWeek(){const today=new Date(dayKey(new Date(state.now||Date.now()))+'T00:00:00+08:00');const weekday=new Date(today.getTime()+8*3600000).getUTCDay();const monday=new Date(today.getTime()+((weekday===0?-6:1-weekday)+offset*7)*86400000);$('#week-title').textContent=`${fmt(monday,{hour:undefined,minute:undefined})} — ${fmt(new Date(+monday+6*86400000),{hour:undefined,minute:undefined})}`;$('#week-grid').innerHTML=Array.from({length:7},(_,i)=>{const d=new Date(+monday+i*86400000),key=dayKey(d);const rows=state.events.filter(e=>['class','exam'].includes(e.kind)&&e.start&&dayKey(new Date(e.start))===key).sort((a,b)=>new Date(a.start)-new Date(b.start));return `<div><div class="day-heading ${key===dayKey(today)?'today':''}">周${'一二三四五六日'[i]}<strong>${key.slice(-2)}</strong></div><div class="day-column">${rows.length?rows.map(e=>`<div class="lesson ${e.kind==='exam'?'exam':''}">${fmt(e.start,{month:undefined,day:undefined})}–${fmt(e.end,{month:undefined,day:undefined})}<b>${escape(e.title)}</b><span>${escape(e.location||names[e.source])}</span></div>`).join(''):'<div class="no-class">暂无日程</div>'}</div></div>`;}).join('');}
function renderSync(){$('#sync-log').innerHTML=state.sync.length?state.sync.map(s=>`<div class="sync-row"><strong>${escape(s.stream)}</strong><small>最近完整成功：${s.success_at?fmt(s.success_at):'尚未成功'}　/　最近尝试：${fmt(s.attempted_at)}</small>${s.error?`<div class="sync-error">${escape(s.error)}</div>`:'<span class="muted">已更新</span>'}</div>`).join(''):empty('尚未同步','按 README 配置连接器后，点击右上角「同步平台」。');}
async function refresh(){if(loading)return;loading=true;try{const r=await fetch('/api/state');if(!r.ok)throw Error('无法读取本地数据');state=await r.json();render();}catch(e){message(e.message);}finally{loading=false;}}
async function post(path,body){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Planner-Token':state.token},body:JSON.stringify(body)});const data=await r.json();if(!r.ok)throw Error(data.error||'操作失败');return data;}
let messageTimer;function message(text){$('#message').hidden=false;$('#message').textContent=text;clearTimeout(messageTimer);messageTimer=setTimeout(()=>$('#message').hidden=true,7000);}
document.addEventListener('click',async event=>{const nav=event.target.closest('[data-view]');if(nav)switchView(nav.dataset.view);const del=event.target.closest('[data-delete]');if(del&&confirm('删除这条手动日程？')){try{await post('/api/delete',{id:del.dataset.delete});await refresh();}catch(e){message(e.message);}}});
document.addEventListener('change',async event=>{if(event.target.dataset.complete){try{await post('/api/complete',{id:event.target.dataset.complete,done:event.target.checked});await refresh();}catch(e){event.target.checked=!event.target.checked;message(e.message);}}});
$('#sync').onclick=async()=>{try{await post('/api/sync',{});await refresh();message('同步已启动，可以继续查看本地日程。');}catch(e){message(e.message);}};
$('#add').onclick=()=>{$('#form-error').textContent='';$('#event-dialog').showModal();};$('#close-dialog').onclick=()=>$('#event-dialog').close();
$('#event-form').onsubmit=async event=>{event.preventDefault();const button=event.submitter;button.disabled=true;try{await post('/api/events',Object.fromEntries(new FormData(event.target)));$('#event-dialog').close();event.target.reset();await refresh();message('日程已添加。');}catch(e){$('#form-error').textContent=e.message;}finally{button.disabled=false;}};
$('#search').oninput=renderTasks;$('#source').onchange=renderTasks;$('#status').onchange=renderTasks;
$('#prev-week').onclick=()=>{offset--;renderWeek();};$('#next-week').onclick=()=>{offset++;renderWeek();};$('#this-week').onclick=()=>{offset=0;renderWeek();};
$('#import').onclick=async()=>{const file=$('#import-file').files[0];if(!file)return message('请先选择 JSON 文件。');if(file.size>2_000_000)return message('文件不能超过 2 MB。');try{const data=JSON.parse((await file.text()).replace(/^\uFEFF/,''));const items=Array.isArray(data)?data:data.events;if(!Array.isArray(items))throw Error('文件必须是数组，或含有 events 数组');await post('/api/import',{source:$('#import-source').value,events:items.map(r=>({...r,kind:r.kind||$('#import-kind').value}))});await refresh();message(`已导入 ${items.length} 条日程。`);}catch(e){message(e.message);}};
refresh();setInterval(refresh,5000);
function renderAuth(){
 const auth=state.auth||{};
 const x=auth.chaoxing,z=auth.zhihuishu;
 if(x)$('#xxt-auth-status').textContent=x.message;
 if(z)$('#zhs-auth-status').textContent=z.message;
 $('#xxt-login button').disabled=!!x&&x.status==='running';
 $('#zhs-login').disabled=!!z&&['running','qr'].includes(z.status);
 const qr=$('#zhs-qr');
 qr.hidden=!(z&&z.status==='qr'&&z.image);
 if(!qr.hidden&&qr.getAttribute('src')!=='data:image/png;base64,'+z.image)qr.src='data:image/png;base64,'+z.image;
 if(qr.hidden)qr.removeAttribute('src');
}
$('#connect').onclick=()=>$('#auth-dialog').showModal();
$('#close-auth').onclick=()=>$('#auth-dialog').close();
$('#xxt-login').onsubmit=async event=>{
 event.preventDefault();const form=event.target,button=form.querySelector('button');button.disabled=true;
 const fields=new FormData(form),body={provider:'chaoxing',phone:fields.get('phone'),password:fields.get('password'),remember:fields.has('remember')};
 try{await post('/api/auth/login',body);form.elements.password.value='';await refresh();}catch(e){$('#xxt-auth-status').textContent=e.message;}finally{body.password='';button.disabled=false;}
};
$('#zhs-login').onclick=async()=>{try{await post('/api/auth/login',{provider:'zhihuishu'});await refresh();}catch(e){$('#zhs-auth-status').textContent=e.message;}};

function renderSubscription(){
 const sub=state.subscription||{}, relay=sub.tunnel||{}, select=$('#subscription-address'), urls=sub.urls||[], previous=select.value;
 select.innerHTML=urls.map(url=>`<option value="${escape(url)}">${url.startsWith('https:')?'HTTPS 临时订阅':'局域网 HTTP'} · ${escape(new URL(url).hostname)}</option>`).join('');
 if(urls.includes(previous)&&select.dataset.httpsOrigin===(relay.origin||''))select.value=previous;
 select.dataset.httpsOrigin=relay.origin||'';
 $('#subscription-url').value=select.value||'';
 $('#copy-subscription').disabled=!urls.length;
 $('#start-https').disabled=!sub.enabled||['starting','ready','reconnecting'].includes(relay.status);
 $('#start-https').textContent=relay.status==='starting'?'正在建立 HTTPS…':'启用 HTTPS 临时链接';
 $('#stop-https').hidden=!['starting','ready','reconnecting'].includes(relay.status);
 $('#https-status').textContent=relay.message||'';
 $('#subscription-status').textContent=sub.error||(!sub.enabled?'订阅服务尚未启动，请重新启动课期。':!urls.length?'未发现局域网地址，请先连接 Wi-Fi 后刷新。':'若有多个地址，选择与手机处于同一网络的地址。');
}
$('#calendar-subscribe').onclick=async()=>{await refresh();$('#subscription-dialog').showModal();};
$('#close-subscription').onclick=()=>$('#subscription-dialog').close();
$('#subscription-address').onchange=()=>{$('#subscription-url').value=$('#subscription-address').value;};
$('#copy-subscription').onclick=async()=>{const field=$('#subscription-url');try{await navigator.clipboard.writeText(field.value);message('链接已复制，请粘贴到手机日历的 URL 导入。');}catch(e){field.focus();field.select();message('请按 Ctrl+C 复制已选中的订阅链接。');}};
$('#reset-subscription').onclick=async()=>{if(!confirm('旧链接会立即失效，手机需重新添加新链接。确定重置？'))return;try{await post('/api/subscription/reset',{});await refresh();message('旧链接已失效，请使用新链接重新订阅。');}catch(e){message(e.message);}};
$('#start-https').onclick=async()=>{try{await post('/api/subscription/https',{enabled:true});await refresh();}catch(e){message(e.message);}};
$('#stop-https').onclick=async()=>{try{await post('/api/subscription/https',{enabled:false});await refresh();}catch(e){message(e.message);}};
