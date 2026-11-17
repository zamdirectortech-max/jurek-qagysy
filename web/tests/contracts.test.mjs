import {test} from 'node:test';
import assert from 'node:assert/strict';
import {firebaseRows,parseCSV} from '../src/data.js';
import {readFileSync} from 'node:fs';
test('Firmware diagnostic fields coexist with metrics and boot clocks are not dates',()=>{const rows=firebaseRows({'-2':{boot_id:'ab',session_id:1,device_uptime_ms:90000,average_bpm:73,mean_rr_ms:821,rmssd_ms:32,sdnn_ms:null,signal_quality:'POOR',fifo_loss_events:2}});assert.equal(rows[0].sdnn,null);assert.match(rows[0].detail,/90000/);assert.equal(rows[0].quality,'POOR');});
test('Malformed Serial lines do not create measurements',()=>assert.deepEqual(parseCSV('startup\n#ERROR sensor\ninvalid,data'),[]));
test('Rules deny root access and use independently administered reader/writer roles',()=>{const r=JSON.parse(readFileSync(new URL('../database.rules.json',import.meta.url))).rules;assert.equal(r['.read'],false);assert.equal(r['.write'],false);assert.equal(r.readers['.write'],false);assert.match(r.hrv_measurements['.read'],/readers/);assert.match(r.hrv_measurements['.read'],/300/);assert.match(r.hrv_measurements.$measurement['.write'],/!data.exists/);});
