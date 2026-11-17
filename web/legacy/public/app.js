import {parseCSV,firebaseRows,demoRow} from './data.js';
const $=id=>document.getElementById(id);
let rows=[], mode='demo', paused=false, index=60, shown=10, generation=0, busy=false;
const format=v=>v===null||v===undefined?'—':v.toLocaleString('ru-RU',{maximumFractionDigits:1});
function notice(text,error=false){$('notice').textContent=text;$('notice').classList.toggle('error',error);}
function chart(id,key,color){
  const data=rows.slice(-Number($('range').value));
  const vals=data.map(r=>r[key]).filter(v=>Number.isFinite(v));
  if(!vals.length){$(id).innerHTML='<div class="empty">Пока нет данных<br><small>Выберите источник или загрузите CSV</small></div>';return;}
  const min=Math.floor(Math.min(...vals)*.9), max=Math.ceil(Math.max(...vals)*1.1)+1;
  const x=i=>48+i*500/Math.max(1,data.length-1), y=v=>180-(v-min)*155/(max-min);
  let path='',pen=false;
  data.forEach((r,i)=>{if(!Number.isFinite(r[key])){pen=false;return;}path+=`${pen?'L':'M'}${x(i)},${y(r[key])} `;pen=true;});
  let grid='';for(let i=0;i<4;i++){const v=min+(max-min)*i/3,yy=y(v);grid+=`<line x1="48" x2="548" y1="${yy}" y2="${yy}" stroke="#edf1ef"/><text x="36" y="${yy+3}" text-anchor="end" fill="#94a49c" font-size="10">${Math.round(v)}</text>`;}
  $(id).innerHTML=`<svg viewBox="0 0 570 210" role="img" aria-label="${key==='bpm'?'Пульс':'RMSSD'}: ${vals.length} точек, от ${format(Math.min(...vals))} до ${format(Math.max(...vals))}">${grid}<path d="${path}" fill="none" stroke="${color}" stroke-width="2.5" stroke-linejoin="round"/>${data.map((r,i)=>Number.isFinite(r[key])?`<circle cx="${x(i)}" cy="${y(r[key])}" r="3" fill="${color}"><title></title></circle>`:'').join('')}<text x="48" y="205" fill="#94a49c" font-size="10">1</text><text x="548" y="205" text-anchor="end" fill="#94a49c" font-size="10">${data.length}</text></svg>`;
  let j=0;data.forEach(r=>{if(Number.isFinite(r[key]))$(id).querySelectorAll('title')[j++].textContent=`${r.label}: ${format(r[key])}`;});
}
function render(){
  const last=rows.at(-1);for(const key of ['bpm','rmssd','rr','sdnn'])$(key).textContent=format(last?.[key]);
  $('count').textContent=rows.length;$('quality').textContent=last?`Статус: ${last.quality}`:'Нет измерений';
  $('rows').replaceChildren();
  for(const r of rows.slice().reverse().slice(0,shown)){const tr=document.createElement('tr');tr.title=r.detail;for(const v of [r.label,format(r.bpm),format(r.rr),format(r.rmssd),r.quality]){const td=document.createElement('td');td.textContent=v;tr.append(td);}$('rows').append(tr);}
  if(!rows.length){const tr=document.createElement('tr'),td=document.createElement('td');td.colSpan=5;td.textContent='Нет измерений. Подключите источник данных.';tr.append(td);$('rows').append(tr);}
  $('export').disabled=!rows.length;$('more').hidden=shown>=rows.length;
  chart('pulseChart','bpm','#409b84');chart('hrvChart','rmssd','#9781bc');
}
async function refresh(){
  if(busy || mode!=='firebase' || paused)return;
  busy=true;const token=generation;
  try{const response=await fetch('/api/measurements',{signal:AbortSignal.timeout(12000)});const data=await response.json();if(token!==generation)return;if(!response.ok)throw new Error(data.error || 'Ошибка сервера');rows=firebaseRows(data);$('status').textContent=`Проверено ${new Date().toLocaleTimeString('ru-RU')}`;notice(rows.length?'Firebase: последние завершённые 60-секундные сессии. Опрос каждые 10 секунд. В прошивке нет календарной метки времени.':'Firebase подключён, но измерений ещё нет. Завершите измерение на плате.');render();}
  catch(e){if(token===generation){$('status').textContent='Нет связи с Firebase';notice(`${e.message} ${rows.length?'На экране сохранены последние полученные данные.':''}`,true);}}
  finally{busy=false;}
}
function switchSource(){generation++;mode=$('source').value;rows=[];shown=10;paused=false;$('pause').textContent='Приостановить';$('pause').hidden=mode==='csv';$('importLabel').hidden=mode!=='csv';$('sourceBadge').textContent={demo:'Демонстрация',firebase:'Firebase',csv:'Serial CSV'}[mode];$('chartCaption').textContent=mode==='firebase'?'Средний пульс по завершённым сессиям · уд/мин':'Последние точки потока · уд/мин';
  if(mode==='demo'){index=60;rows=Array.from({length:60},(_,i)=>demoRow(i));$('status').textContent='Синтетический поток · каждые 2 с';notice('Демо-режим: показатели сгенерированы и не поступают с устройства.');}
  else if(mode==='csv'){$('status').textContent='Ожидание файла';notice('Загрузите CSV или текстовый журнал Serial Monitor: time_s,IR,BPM,Avg_BPM,RR_ms,RMSSD_ms,Status.');}
  else{$('status').textContent='Подключение…';notice('Чтение Firebase через локальный сервер…');refresh();}render();}
$('source').addEventListener('change',switchSource);
$('range').addEventListener('change',render);
$('pause').addEventListener('click',()=>{paused=!paused;$('pause').textContent=paused?'Продолжить':'Приостановить';$('status').textContent=paused?'Обновление приостановлено':mode==='demo'?'Синтетический поток · каждые 2 с':'Обновление возобновлено';if(!paused)refresh();});
$('more').addEventListener('click',()=>{shown+=20;render();});
$('file').addEventListener('change',async e=>{const file=e.target.files[0];if(!file)return;const token=generation;try{if(file.size>5*1024*1024)throw new Error('Максимальный размер файла — 5 МБ.');const parsed=parseCSV(await file.text());if(token!==generation)return;if(!parsed.length)throw new Error('Не найдены строки CSV в формате прошивки.');rows=parsed.slice(-300);shown=10;$('status').textContent=`Импортировано: ${parsed.length} строк`;notice('Данные из файла. Время — секунды от начала измерения; SDNN отсутствует в Serial CSV.');render();}catch(err){notice(err.message,true);}finally{e.target.value='';}});
$('export').addEventListener('click',()=>{const escape=v=>'"'+String(v??'').replace(/"/g,'""').replace(/^[=+@-]/,"'$&")+'"';const csv=['source,point,bpm,rr_ms,rmssd_ms,sdnn_ms,status',...rows.map(r=>[mode,r.label,r.bpm,r.rr,r.rmssd,r.sdnn,r.quality].map(escape).join(','))].join('\r\n');const a=document.createElement('a');a.href=URL.createObjectURL(new Blob(['\uFEFF'+csv],{type:'text/csv;charset=utf-8'}));a.download=`jurek-${mode}.csv`;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);});
setInterval(()=>{if(mode==='demo'&&!paused){rows.push(demoRow(index++));rows=rows.slice(-300);render();}},2000);
setInterval(refresh,10000);switchSource();
