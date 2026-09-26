// Run against a launched local package. Never commit private ULog fixtures.
// REPLAY_URL=http://127.0.0.1:.../token/ REPLAY_LOG=/path/flight.ulg node ...
const {chromium,webkit}=require('playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');

(async()=>{
 const url=process.env.REPLAY_URL, log=process.env.REPLAY_LOG;
 assert(url&&log,'REPLAY_URL and REPLAY_LOG are required');
 const out=process.env.REPLAY_EVIDENCE||'/tmp/flight-replay-evidence';fs.mkdirSync(out,{recursive:true});
 const browser=process.env.REPLAY_BROWSER==='webkit' ? await webkit.launch({headless:true}) : await chromium.launch({channel:'chrome',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1440,height:1000},deviceScaleFactor:1});
  await page.bringToFront();
  const errors=[], external=[], uploads=[];
  page.on('pageerror',e=>errors.push(e.message));
  page.on('request',r=>{
   if(new URL(r.url()).origin!==new URL(url).origin)external.push(r.url());
   if(r.method()!=='GET'&&r.method()!=='HEAD')uploads.push(r.url());
  });
  const results={browser:process.env.REPLAY_BROWSER||'chrome',checks:[]};
  const check=s=>{results.checks.push(s);console.log('PASS',s)};
  await page.goto(url);
  await page.waitForFunction(()=>!document.getElementById('open').disabled,null,{timeout:30000});
  // Real OS file-picker event, then choose the provided Unicode-path file.
  const chooser=page.waitForEvent('filechooser');await page.locator('#open').click({delay:80});
  await (await chooser).setFiles(log);
  await page.waitForFunction(()=>!document.getElementById('play').disabled,null,{timeout:30000});
  assert.equal(await page.locator('#filename').textContent(),path.basename(log));
  results.fileMeta=await page.locator('#filemeta').textContent();
  check('file picker and ULog load');
  const position=async()=>Number(await page.locator('#seek').inputValue());
  const duration=Number(await page.locator('#seek').getAttribute('max'));
  assert(duration>1);results.duration=duration;
  await page.getByRole('button',{name:'播放',exact:true}).click({delay:80});
  await page.waitForFunction(()=>Number(document.getElementById('seek').value)>.4,null,{timeout:10000});
  await page.getByRole('button',{name:'暫停',exact:true}).click({delay:80});
  await page.getByRole('button',{name:'播放',exact:true}).waitFor();
  const paused=await position();await page.waitForTimeout(400);assert(Math.abs(await position()-paused)<.04);
  check('play and pause');
  async function drag(fraction) {
   const box=await page.locator('#seek').boundingBox();
   await page.mouse.move(box.x+box.width*.2,box.y+box.height/2);
   await page.mouse.down();
   await page.mouse.move(box.x+8+(box.width-16)*fraction,box.y+box.height/2,{steps:8});
   await page.mouse.up();await page.waitForTimeout(200);
   assert(Math.abs(await position()-duration*fraction)<duration*.02);
  }
  await drag(.75);await drag(.25);assert.equal(await page.locator('#play').getAttribute('aria-label'),'播放');
  check('mouse seeks forward/backward and preserves pause');
  await page.screenshot({path:path.join(out,'replay.png')});
  await page.getByRole('button',{name:'播放',exact:true}).click({delay:80});
  // Wait for the action to take effect before starting another gesture.
  await page.getByRole('button',{name:'暫停',exact:true}).waitFor();
  await drag(.5);
  assert.equal(await page.locator('#play').getAttribute('aria-label'),'暫停');
  check('drag restores playing state');
  await page.locator('#canvas').focus();await page.keyboard.press('Space');await page.waitForTimeout(200);
  assert.equal(await page.locator('#play').getAttribute('aria-label'),'播放');
  check('keyboard and HTML controls stay synchronized');
  await drag(1);
  await page.getByRole('button',{name:'播放',exact:true}).click({delay:80});
  await page.waitForTimeout(300);assert(await position()<2);
  await page.getByRole('button',{name:'暫停',exact:true}).click({delay:80});
  await page.getByRole('button',{name:'播放',exact:true}).waitFor();
  check('play at end restarts the flight');
  const before=await position();
  // Valid magic with no usable samples exercises parser failure, not only UI validation.
  await page.setInputFiles('#file',{name:'invalid.ulg',mimeType:'application/octet-stream',buffer:Buffer.from([85,76,111,103,1,18,53,1,...Array(24).fill(0)])});
  await page.waitForFunction(()=>document.getElementById('status').textContent==='保留原回放');
  assert(Math.abs(await position()-before)<.04);
  assert.equal(await page.locator('#filename').textContent(),path.basename(log));
  check('bad ULog preserves previous flight');
  if(process.env.REPLAY_SECOND_LOG) {
   await page.setInputFiles('#file',process.env.REPLAY_SECOND_LOG);
   await page.waitForFunction(name=>document.getElementById('filename').textContent===name,path.basename(process.env.REPLAY_SECOND_LOG));
   assert(await position()<.05);check('replacement ULog resets timeline');
  }
  await page.setInputFiles('#file',log);
  await page.waitForFunction(name=>document.getElementById('filename').textContent===name&&document.getElementById('status').textContent==='就緒',path.basename(log));
  await page.locator('#restart').click({delay:80});assert(await position()<.05);check('reload and restart');
  await page.setViewportSize({width:900,height:700});await page.waitForTimeout(300);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));check('window resize');
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);assert.deepEqual(uploads,[]);
  check('no script errors, external requests or ULog uploads');
  fs.writeFileSync(path.join(out,'browser-results.json'),JSON.stringify(results,null,2));
 } finally {await browser.close()}
})().catch(e=>{console.error(e);process.exit(1)});
