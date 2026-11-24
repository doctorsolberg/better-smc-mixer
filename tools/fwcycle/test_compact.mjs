import test from 'node:test';
import assert from 'node:assert/strict';
import { compactSnapshot } from './editor.mjs';
test('large install diagnostics retain guards within RPC response bound',()=>{
 const plan={failed:0,view:'stock',writes:[{what:'our area',offset:266240,size:173608}],items:Array(200).fill({text:'x'.repeat(1000)})};
 const state={job:{plan,preview:{valid:true,plan,files:{backup:{size:524288,sha256:'a'}}}},latest:{plan:{plan},complete:{sequence:9}},events:Array(128).fill({plan}),identity:{connected:false}};
 const result=compactSnapshot(state);
 assert.ok(Buffer.byteLength(JSON.stringify(result))<20000);
 assert.equal(result.job.plan.failed,0);
 assert.equal(result.job.preview.valid,true);
 assert.equal(result.latest.complete.sequence,9);
 assert.equal(state.events.length,128);
});
