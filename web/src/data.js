export function number(value) { return value === null || value === undefined || String(value).trim() === '' || !Number.isFinite(Number(value)) ? null : Number(value); }
export function firebaseRows(data) {
  if(data === null) return [];
  if(typeof data !== 'object' || Array.isArray(data)) throw new Error('Неожиданный формат Firebase.');
  return Object.entries(data).sort(([a],[b])=>a.localeCompare(b)).filter(([,v])=>v && typeof v === 'object').map(([id,v],i)=>({id,label:`Сессия ${v.session_id ?? i+1}`,bpm:number(v.average_bpm),rr:number(v.mean_rr_ms),rmssd:number(v.rmssd_ms),sdnn:number(v.sdnn_ms),quality:String(v.signal_quality ?? '—'),detail:`boot ${v.boot_id ?? '—'} · uptime ${v.device_uptime_ms ?? '—'} мс`}));
}
export function parseCSV(text) {
  let session=0, previous=-1;
  return text.replace(/^\uFEFF/,'').split(/\r?\n/).flatMap(line=>{
    const c=line.trim().split(',');
    if(c.length!==7 || !/^\d+(\.\d+)?$/.test(c[0])) return [];
    const t=number(c[0]);
    if(t<previous) session++; previous=t;
    return [{id:`csv-${session}-${t}`,label:`${session+1} / ${t.toFixed(1)} с`,bpm:number(c[2]),rr:number(c[4]),rmssd:number(c[5]),sdnn:null,ir:number(c[1]),quality:c[6],detail:`Средний пульс: ${c[3]} уд/мин`}];
  });
}
export function demoRow(i) {
  const bpm=70+5*Math.sin(i*.23)+2*Math.sin(i*.71);
  return {id:`demo-${i}`,label:`Демо ${i+1}`,bpm,rr:60000/bpm,rmssd:36+7*Math.sin(i*.15),sdnn:44+4*Math.cos(i*.12),quality:'DEMO',detail:'Синтетический пример · не измерение',ir:92000+1200*Math.sin(i*.6)};
}

