"""Bounded JSON-lines IPC to the standalone CDP adapter.

Closing this bridge stops only its child process, never the browser or an editor flash job.
"""
from model import GuardError
import json
from pathlib import Path
import os
import select
import subprocess
import threading
import time

class BrowserAdapter:
    """Compose explicit UI actions and completed private downloads; never flash directly."""
    def __init__(self, endpoint, origin, run_dir, rpc=None, profile=None):
        self.root=Path(run_dir).resolve()
        self.downloads=self.root/'downloads'
        self.downloads.mkdir(mode=0o700,exist_ok=True)
        if rpc is None and not profile:raise ValueError('owned Chrome profile required')
        self.rpc=rpc or NodeBridge().__enter__()
        self.owned=rpc is None
        self.poll_interval=1.0
        self.deadline=600
        self.pin=None
        self.power_acknowledged=False
        self.port_reader=None
        if self.owned:
            from macos import midi_endpoints
            self.port_reader=midi_endpoints
            assert profile is not None
            try:
                self.rpc.call('attach',{'endpoint':endpoint,'origin':origin,'downloads':str(self.downloads),'profile':str(Path(profile).resolve())})
            except BaseException:
                self.rpc.__exit__(None,None,None)
                raise
    def close(self):
        if self.owned:
            self.rpc.__exit__(None,None,None)
    def wait(self,predicate,seconds=None):
        until=time.monotonic()+(seconds or self.deadline)
        while True:
            state=self.snapshot()
            if (state.get('job') or {}).get('error'):raise RuntimeError(str(state['job']['error']))
            if predicate(state):return state
            if time.monotonic()>=until:raise TimeoutError('editor state deadline; action outcome may be unknown')
            time.sleep(self.poll_interval)
    def snapshot(self):
        state=self.rpc.call('snapshot')
        install=state.get('install',{})
        state['restart']={'verified':False,'binding':install.get('binding')}
        done=install.get('completion_sequence')
        connection=state.get('latest',{}).get('connection',{}).get('sequence',0)
        if (self.power_acknowledged and type(done) is int and done>0
                and connection>done and install.get('completed') is True
                and state.get('identity',{}).get('kind')=='ours'):
            state['restart']['verified']=True
        if state.get('backup',{}).get('complete'):
            preview=(state.get('job') or {}).get('preview',{}).get('files',{}).get('backup',{})
            found=self.completed_download(size=preview.get('size'),digest=preview.get('sha256')) if preview else None
            state['backup']['complete']=found is not None
            state['backup']['path']=str(found) if found else None
        return state
    def release(self,pin):
        self.rpc.call('release',{'manifest':pin['manifest'], **({'manifest_sha256':pin['manifest_sha256']} if pin.get('manifest_sha256') else {})})
        self.pin=pin
        return {'matched':True}
    def completed_download(self,name=None,size=None,digest=None,exclude=(),record_only=False):
        import hashlib
        receipt_path=self.root/'download-receipts.json'
        if receipt_path.is_symlink():raise ValueError('symlink receipt file refused')
        receipts=json.loads(receipt_path.read_text()) if receipt_path.exists() else {}
        live=self.rpc.call('downloads').get('downloads',[])
        for row in live:
            if row.get('state')!='completed':continue
            guid=row.get('guid','')
            if not isinstance(guid,str) or not guid or Path(guid).name!=guid:continue
            path=self.downloads/guid
            if path.is_symlink() or not path.is_file():continue
            content=path.read_bytes()
            if guid in receipts:continue
            receipts[guid]={**row,'sha256':hashlib.sha256(content).hexdigest(),'size':len(content)}
        if live:
            from evidence import Evidence
            writer=Evidence.__new__(Evidence);writer.root=self.root;writer.lock_fd=None
            writer.write_json('download-receipts.json',receipts)
        if record_only:return None
        matches=[]
        for row in receipts.values():
            if row.get('state')!='completed' or row.get('guid') in exclude:continue
            if name and row.get('name')!=name:continue
            guid=row.get('guid','')
            if not isinstance(guid,str) or not guid or Path(guid).name!=guid:continue
            p=self.downloads/guid
            if p.is_symlink() or not p.is_file():continue
            data=p.read_bytes()
            if len(data)!=row.get('size') or hashlib.sha256(data).hexdigest()!=row.get('sha256'):continue
            if size is not None and len(data)!=size:continue
            if digest and hashlib.sha256(data).hexdigest()!=digest:continue
            matches.append(p)
        if len(matches)>1:raise ValueError('ambiguous completed download')
        return matches[0] if matches else None
    def close_dialog(self):
        if self.snapshot().get('dom',{}).get('dialogOpen'):
            self.rpc.call('action',{'name':'close_dialog'})
    def connection_readiness(self,name,transport):
        """Prerequisite-only check: never dispatch the editor's Connect action."""
        if transport not in {'usb','ble'}:raise ValueError('explicit transport required')
        if name not in {'stockConnect','oursConnect'}:raise ValueError('connection action required')
        stock=name=='stockConnect'
        endpoint='SMC-Mixer -Private' if stock else ('SMC-Mixer Config' if transport=='usb' else 'Better-SMC-Mixer Bluetooth')
        if self.owned and transport=='ble':
            from macos import ensure_midi
            native=ensure_midi(transport,'stock' if stock else 'ours')
            if native.get('ready') is not True:return native
        if self.port_reader and (transport=='usb' or not stock):
            end=time.monotonic()+30
            while True:
                try:
                    ports=self.port_reader()
                except (TimeoutError, subprocess.TimeoutExpired):
                    return {'ready':False,'kind':'midi_enumeration',
                            'message':'Native MIDI endpoint enumeration timed out; inspect prerequisites before rechecking.'}
                if all(endpoint in ports.get(k,[]) for k in ('inputs','outputs')):break
                if time.monotonic()>=end:
                    return {'ready':False,'kind':'native_connection','expected_endpoint':endpoint,
                            'message':'Exact native MIDI endpoint did not enumerate: '+endpoint}
                time.sleep(self.poll_interval)
        return {'ready':True}
    def action(self,name,transport,**params):
        if transport not in {'usb','ble'}:raise ValueError('explicit transport required')
        t=transport.upper()
        if name in {'stockConnect','oursConnect'}:
            if params.get('readiness_checked') is not True:
                readiness=self.connection_readiness(name,transport)
                if readiness.get('ready') is not True:return readiness
            stock=name=='stockConnect'
            endpoint='SMC-Mixer -Private' if stock else ('SMC-Mixer Config' if transport=='usb' else 'Better-SMC-Mixer Bluetooth')
            return self.rpc.call('action',{'name':'stock_connect' if stock else 'ours_connect','transport':t,'endpoint':endpoint})
        if name=='rollback':return self.rpc.call('action',{'name':'rollback','transport':t})
        if name=='backup':
            self.close_dialog()
            return self.rpc.call('action',{'name':'backup_prepare','transport':t})
        if name=='install':
            binding=params.get('binding')
            if not binding or self.snapshot().get('backup',{}).get('binding') != binding:
                raise GuardError('install backup job/page binding mismatch before UI dispatch')
            self.rpc.call('action',{'name':'backup_next','transport':t,'binding':binding})
            return self.rpc.call('action',{'name':'install','transport':t,'binding':binding})
        raise ValueError('unsupported UI action: '+name)
    def prepare_rollback(self):
        self.close_dialog()
        self.rpc.call('action',{'name':'rollback_prepare'})
        return self.wait(lambda s:s.get('rollback',{}).get('ready'),30)
    def export_config(self,name):
        self.close_dialog()
        before=self.snapshot().get('panelObservation',{})
        read_sequence=before.get('readSequence',0)
        epoch=before.get('connectionEpoch')
        self.rpc.call('action',{'name':'baseline' if name=='before' else 'final_export'})
        self.wait(lambda s:s.get('config',{}).get('pulled') is True
                  and s.get('panelObservation',{}).get('fresh') is True
                  and s['panelObservation'].get('connectionEpoch')==epoch
                  and s['panelObservation'].get('readSequence',0)>read_sequence,30)
        self.close_dialog()
        self.completed_download(record_only=True)  # Persist receipts observed before this request.
        receipt_path=self.root/'download-receipts.json'
        exclude=set(json.loads(receipt_path.read_text())) if receipt_path.exists() else set()
        exclude.update(row.get('guid') for row in self.rpc.call('downloads').get('downloads',[]))
        self.rpc.call('exportConfig',{'name':name})
        end=time.monotonic()+min(30,self.deadline)
        while time.monotonic()<end:
            p=self.completed_download(name=name+'.json',exclude=exclude)
            if p:return p
            time.sleep(self.poll_interval)
        raise TimeoutError('configuration download did not complete')

class NodeBridge:
    def __init__(self, command=None, timeout=45):
        self.command=command or ['node',str(Path(__file__).with_name('bridge.mjs'))]
        self.timeout=timeout
        self.process=None
        self.sequence=0
        self.lock=threading.Lock()
        self.buffer=b''
        self.poisoned=False
    def __enter__(self):
        self.process=subprocess.Popen(self.command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
        return self
    def __exit__(self,*args):
        if self.process:
            if self.process.poll() is None:
                self.process.terminate()
                try:self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=3)
            if self.process.stdin:self.process.stdin.close()
            if self.process.stdout:self.process.stdout.close()
    def call(self, method, params=None):
        with self.lock:
            if self.poisoned:raise RuntimeError('bridge needs reconciliation after failed RPC')
            if not self.process or self.process.poll() is not None:raise RuntimeError('bridge not running')
            if not self.process.stdin or not self.process.stdout:raise RuntimeError('bridge pipes unavailable')
            self.sequence+=1
            request={'id':self.sequence,'method':method,'params':params or {}}
            try:
                self.process.stdin.write((json.dumps(request)+'\n').encode())
                self.process.stdin.flush()
                deadline=time.monotonic()+self.timeout
                while b'\n' not in self.buffer:
                    left=deadline-time.monotonic()
                    if left<=0 or not select.select([self.process.stdout],[],[],left)[0]:
                        raise TimeoutError('RPC outcome unknown: '+method)
                    chunk=os.read(self.process.stdout.fileno(),65536)
                    if not chunk:raise RuntimeError('bridge closed before RPC outcome')
                    self.buffer+=chunk
                    if len(self.buffer)>2*1024*1024:raise RuntimeError('oversized bridge response')
                raw,self.buffer=self.buffer.split(b'\n',1)
                response=json.loads(raw)
                if response.get('id')!=self.sequence:raise RuntimeError('unexpected RPC response id')
                if 'error' in response:raise RuntimeError(str(response['error']))
                if 'result' not in response:raise RuntimeError('missing RPC result')
                return response['result']
            except BaseException:
                self.poisoned=True
                raise
