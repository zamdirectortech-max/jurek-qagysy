import {test} from 'node:test';
import assert from 'node:assert/strict';
import {parseCSV,firebaseRows,number} from './src/data.js';
test('Serial logs, missing metrics and session reset',()=>{const r=parseCSV('time_s,IR,BPM,Avg_BPM,RR_ms,RMSSD_ms,Status\n#CAL RR=800\n2.00,90000,75,74,800,nan,GOOD\n0.10,91000,nan,74,0,32,WAIT');assert.equal(r.length,2);assert.equal(r[0].rmssd,null);assert.equal(r[1].bpm,null);assert.equal(r[1].label,'2 / 0.1 с');});
test('Firebase firmware schema and empty database',()=>{assert.deepEqual(firebaseRows(null),[]);const r=firebaseRows({'-B':{average_bpm:72,mean_rr_ms:833,rmssd_ms:35,sdnn_ms:42,session_id:2,signal_quality:'GOOD'},'-A':{average_bpm:null}});assert.equal(r[0].bpm,null);assert.equal(r[1].sdnn,42);assert.equal(r[1].label,'Сессия 2');assert.throws(()=>firebaseRows('bad'));});
test('Missing values are not fabricated zeros',()=>{for(const v of [null,undefined,'','nan','Infinity'])assert.equal(number(v),null);assert.equal(number('0'),0);});

