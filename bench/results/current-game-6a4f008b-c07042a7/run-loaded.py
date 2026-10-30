import os, pathlib, subprocess, time, signal, json, hashlib, re
D=pathlib.Path('/tmp/bender-current-game-ab-evidence'); G=D/'game-source'
arms=[('baseline','/tmp/bender-current-game-ab-6a4f008b/build/bender'),('tip','/tmp/bender-current-game-ab-c07042a7/build/bender')]
rows=[]
for tag,exe in arms:
 env={k:v for k,v in os.environ.items() if not k.startswith(('BENDER_','BENDER_'))}
 env.update(BENDER_NO_PROFILE='1',BENDER_LANES='0',XDG_CACHE_HOME=str(D/('cache-loaded-'+tag)))
 cmd=[exe,str(G/'main.ben'),'--frames','600','--uncapped','--world-slot','15f2268da8ec2a4105ef2deaf6ae0b19','--world-root',str(G/'worlds-v1')]
 start=time.monotonic(); peak=0; reason=None
 with (D/(tag+'-loaded.stdout')).open('w') as o,(D/(tag+'-loaded.stderr')).open('w') as e:
  p=subprocess.Popen(cmd,cwd=G,env=env,stdout=o,stderr=e,start_new_session=True)
  while p.poll() is None:
   q=subprocess.run(['/bin/ps','-o','rss=','-p',str(p.pid)],capture_output=True,text=True).stdout.strip()
   if q: peak=max(peak,int(q))
   if peak>1048576: reason='sampled RSS > 1 GiB'
   if time.monotonic()-start>90: reason='wall > 90s'
   if reason: os.killpg(p.pid,signal.SIGKILL); break
   try:p.wait(timeout=.25)
   except subprocess.TimeoutExpired:pass
  rc=p.wait()
 text=(D/(tag+'-loaded.stdout')).read_text()+'\n'+(D/(tag+'-loaded.stderr')).read_text()
 m=re.search(r'^frames:\s*(\d+)',text,re.M)
 row=dict(tag=tag,argv=cmd,exit=rc,termination_reason=reason,wall_s=time.monotonic()-start,peak_sampled_rss_kib=peak,binary_sha256=hashlib.sha256(pathlib.Path(exe).read_bytes()).hexdigest(),actual_frames=int(m[1]) if m else None,summary=[s for s in text.splitlines() if any(k in s for k in ('frames:','steady:','worst frame:','scene:','worldgen:','loaded','error:'))])
 row['completed']=rc==0 and reason is None and row['actual_frames']==600
 rows.append(row);(D/(tag+'-loaded.json')).write_text(json.dumps(row,indent=2)); print(json.dumps(row),flush=True)
(D/'loaded-comparison.json').write_text(json.dumps(rows,indent=2))
raise SystemExit(0 if all(r['completed'] for r in rows) else 1)
