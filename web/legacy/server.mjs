import http from 'node:http';
import {readFile} from 'node:fs/promises';
const files = {'/':'index.html','/app.js':'app.js','/data.js':'data.js','/style.css':'style.css'};
const types = {html:'text/html; charset=utf-8',js:'text/javascript; charset=utf-8',css:'text/css; charset=utf-8'};
const server = http.createServer(async (req,res) => {
  res.setHeader('Cache-Control','no-store');
  res.setHeader('X-Content-Type-Options','nosniff');
  try {
    const path = new URL(req.url,'http://localhost').pathname;
    if (req.method !== 'GET') {res.writeHead(405); return res.end();}
    if(path === '/api/measurements') {
      res.setHeader('Content-Type','application/json');
      if(!process.env.FIREBASE_DATABASE_URL) {res.writeHead(503); return res.end(JSON.stringify({error:'Firebase не настроен. Заполните web/.env и перезапустите сервер.'}));}
      const url = new URL(process.env.FIREBASE_DATABASE_URL);
      if(url.protocol !== 'https:' || !/\.(firebaseio\.com|firebasedatabase\.app)$/.test(url.hostname)) throw new Error('configuration');
      url.pathname = '/hrv_measurements.json';
      url.search = '';
      url.searchParams.set('orderBy','"$key"'); url.searchParams.set('limitToLast','300');
      if(process.env.FIREBASE_AUTH_TOKEN) url.searchParams.set('auth',process.env.FIREBASE_AUTH_TOKEN);
      const upstream = await fetch(url,{signal:AbortSignal.timeout(10000)});
      if(!upstream.ok) {res.writeHead(502); return res.end(JSON.stringify({error:`Firebase HTTP ${upstream.status}. Проверьте URL, токен и правила чтения.`}));}
      return res.end(JSON.stringify(await upstream.json()));
    }
    if(!files[path]) {res.writeHead(404); return res.end('Not found');}
    res.setHeader('Content-Type',types[files[path].split('.').pop()]);
    res.end(await readFile(new URL('./public/'+files[path],import.meta.url)));
  } catch {res.writeHead(502,{'Content-Type':'application/json'}); res.end(JSON.stringify({error:'Не удалось получить данные. Проверьте сеть и настройки сервера.'}));}
});
server.listen(Number(process.env.PORT || 3000),'127.0.0.1',()=>console.log(`Jurek Monitor: http://localhost:${server.address().port}`));
