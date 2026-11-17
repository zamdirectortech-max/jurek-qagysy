import {initializeApp} from 'firebase/app';
import {getAuth, setPersistence, browserSessionPersistence} from 'firebase/auth';
import {getDatabase} from 'firebase/database';
const e=import.meta.env;
export const config={apiKey:e.VITE_FIREBASE_API_KEY,authDomain:e.VITE_FIREBASE_AUTH_DOMAIN,databaseURL:e.VITE_FIREBASE_DATABASE_URL,projectId:e.VITE_FIREBASE_PROJECT_ID,appId:e.VITE_FIREBASE_APP_ID};
export const configured=Object.values(config).every(Boolean);
export let auth=null,db=null,setupError='';
export let authReady=Promise.resolve();
if(configured){try{const app=initializeApp(config);auth=getAuth(app);db=getDatabase(app);authReady=setPersistence(auth,browserSessionPersistence);authReady.catch(()=>{});}catch{setupError='Некорректная конфигурация Firebase. Проверьте web/.env.';}}
