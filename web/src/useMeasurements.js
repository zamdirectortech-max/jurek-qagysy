import {useEffect,useState} from 'react';
import {onAuthStateChanged} from 'firebase/auth';
import {ref,query,orderByKey,limitToLast,onValue} from 'firebase/database';
import {auth,db,configured,setupError} from './firebase';
import {firebaseRows,demoRow} from './data';
export function useMeasurements(mode,retry){
 const [user,setUser]=useState(null),[authLoading,setAuthLoading]=useState(Boolean(auth));
 const [state,setState]=useState({rows:[],loading:false,error:'',connected:false});
 useEffect(()=>auth?onAuthStateChanged(auth,u=>{setUser(u);setAuthLoading(false);}):undefined,[]);
 useEffect(()=>{
  let active=true; const update=p=>{if(active)setState(s=>({...s,...p}));};
  setState({rows:[],loading:false,error:'',connected:false});
  if(mode==='demo'){let i=36;update({rows:Array.from({length:i},(_,n)=>demoRow(n))});const timer=setInterval(()=>{const row=demoRow(i++);setState(s=>({...s,rows:[...s.rows,row].slice(-300)}));},4000);return()=>{active=false;clearInterval(timer);};}
  if(mode!=='firebase')return;
  if(!configured||setupError){update({error:setupError||'Firebase не настроен. Заполните web/.env по примеру .env.example и перезапустите приложение.'});return;}
  if(!user)return;
  update({loading:true});
  const timeout=setTimeout(()=>update({loading:false,error:'Данные пока не получены. Проверьте соединение; подписка продолжает ожидать.'}),15000);
  const offConnection=onValue(ref(db,'.info/connected'),snap=>update({connected:snap.val()===true}));
  const off=onValue(query(ref(db,'hrv_measurements'),orderByKey(),limitToLast(300)),snap=>{clearTimeout(timeout);try{update({rows:firebaseRows(snap.val()),loading:false,error:''});}catch(e){update({loading:false,error:e.message});}},()=>{clearTimeout(timeout);update({loading:false,error:'Нет доступа к измерениям. Проверьте readers/<UID> и правила Firebase. Последние данные могут быть устаревшими.'});});
  return()=>{active=false;clearTimeout(timeout);off();offConnection();};
 },[mode,user,retry]);
 return {...state,user,authLoading};
}

