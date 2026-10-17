"""Real-board acceptance. Requires an empty history; never runs a real Windows client.

USB represents the second computer for routing tests. Two favorite proof
records remain after success for a physical power-cycle check. The Mac BLE upload and
download are real. Native Windows and simultaneous Mac+Windows BLE links must
be checked on Windows hardware separately.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import time
import uuid

from clipboard_agent.ble_transport import BLETransport
from clipboard_agent.device_link import candidate_ports
from clipboard_agent.hub_client import load_config, support_dir
from clipboard_agent.hub_protocol import CHUNK, HubError, make_upload
from clipboard_agent.hub_transport import USBTransport, upload


class USBPeer:
    def __init__(self, link, token): self.link, self.token = link, token
    def request(self, data): return self.link.request({**data, "token": self.token})
    def chunk(self, event, part, offset, data):
        return self.request({"op":"chunk", "event_id":event, "part":part,
            "offset":offset, "data":base64.b64encode(data).decode()})
    def download(self, job, offset, size):
        return base64.b64decode(self.request({"op":"fetch", "job_id":job,
            "offset":offset, "size":size})["data"], validate=True)


class Acceptance:
    def __init__(self, config, minutes, report):
        self.config, self.minutes, self.report = config, minutes, report
        self.link = USBTransport(candidate_ports()[0])
        self.admin = USBPeer(self.link, config["token"])
        self.ble = None
        self.ids = set()
        self.peer_id = str(uuid.uuid4())
        self.peer = None
        self.last_heartbeat = 0
        self.proof = set()
        self.success = False
        self.sessions = [str(uuid.uuid4()), str(uuid.uuid4())]
        self.results = {"transport":"ble", "content":"text_only", "windows":"USB simulated peer; native Windows unverified", "checks":[], "samples":[]}

    def note(self, name, **data):
        print(time.strftime("%H:%M:%S"), name, json.dumps(data, ensure_ascii=False), flush=True)
        self.results["checks"].append({"check":name, **data})
        self.report.parent.mkdir(parents=True, exist_ok=True)
        self.report.write_text(json.dumps(self.results, ensure_ascii=False, indent=2))

    def heartbeat(self, force=False):
        if not force and time.monotonic() - self.last_heartbeat < 2:
            return
        self.ble.request({"op":"heartbeat", "session":self.sessions[0]})
        self.peer.request({"op":"heartbeat", "session":self.sessions[1]})
        self.last_heartbeat = time.monotonic()

    def put(self, text, favorite=False, virtual=False):
        item=make_upload(self.peer_id if virtual else self.config["device_id"], "Acceptance",
                         text=text, favorite=favorite)
        start=time.monotonic()
        chunks=[min(CHUNK,len(data)-offset) for data in (item.content,item.card) for offset in range(0,len(data),CHUNK)]
        count=0;reported=start
        def progress():
            nonlocal count,reported
            count+=1;self.heartbeat()
            if time.monotonic()-reported>=10:
                self.note("upload_progress",bytes=sum(chunks[:count]),total=sum(chunks),seconds=round(time.monotonic()-start))
                reported=time.monotonic()
        record=upload(self.peer if virtual else self.ble, item, progress=progress)
        self.ids.add(record)
        self.note("upload", kind=item.metadata["kind"], bytes=len(item.content), record=record,
                  seconds=round(time.monotonic()-start,2), virtual_source=virtual)
        return record,item

    def receive(self, record, expected, virtual=False, chip=None):
        self.heartbeat(force=True)
        target=self.peer_id if virtual else self.config["device_id"]
        receiver=self.peer if virtual else self.ble
        self.admin.request({"op":"test_select", "record_id":record, "kind":int(chip is not None),
                            "chip":chip or 0, "target":target})
        job=receiver.request({"op":"poll"}).get("job")
        assert job, "target did not receive selected job"
        start=time.monotonic();reported=start;data=bytearray()
        while len(data)<job["bytes"]:
            self.heartbeat()
            block=receiver.download(job["id"],len(data),min(CHUNK,job["bytes"]-len(data)))
            assert block, "empty download"
            data.extend(block)
            if time.monotonic()-reported>=10:
                self.note("download_progress",record=record,bytes=len(data),total=job["bytes"],seconds=round(time.monotonic()-start))
                reported=time.monotonic()
        assert bytes(data)==expected, "original bytes differ"
        assert hashlib.sha256(data).hexdigest()==job["sha256"]
        receiver.request({"op":"ready", "job_id":job["id"]})
        receiver.request({"op":"ack", "job_id":job["id"], "success":True})
        assert not receiver.request({"op":"poll"}).get("job")
        self.note("download_exact", record=record, bytes=len(data), virtual_target=virtual,
                  seconds=round(time.monotonic()-start,2))
        return bytes(data)

    def restart(self):
        self.ble.close();self.ble=None
        self.admin.request({"op":"restart"});self.link.close()
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            time.sleep(1)
            try:
                ports=candidate_ports()
                if not ports: continue
                self.link=USBTransport(ports[0]);self.admin=USBPeer(self.link,self.config["token"])
                if self.admin.request({"op":"status"})["storage_ready"]:break
                self.link.close()
            except (OSError,TimeoutError): pass
        else: raise AssertionError("board did not restart")
        self.ble=BLETransport(self.config)
        self.peer=USBPeer(self.link,self.peer.token)
        self.sessions=[str(uuid.uuid4()),str(uuid.uuid4())]
        self.heartbeat(force=True)
        self.continuous_start=time.monotonic()

    def run(self):
        assert self.admin.request({"op":"status"}).get("content")=="text_only", "Text-only firmware required"
        assert not self.admin.request({"op":"list"})["records"], "Acceptance requires an empty board, before migration"
        self.admin.request({"op":"pair_window"})
        peer=self.link.request({"op":"pair","device_id":self.peer_id,"name":"USB test peer","platform":"windows"})
        self.peer=USBPeer(self.link,peer["token"])
        self.ble=BLETransport(self.config);self.heartbeat()
        self.note("BLE_connected", fragment=self.ble.fragment)
        text="[验收] 中文\n换行与表情 🏓🙂\n电话 13800138000"
        record,item=self.put(text=text,favorite=True)
        assert upload(self.ble,item)==record
        assert len(self.ble.request({"op":"list"})["records"])==1
        self.receive(record,item.content,virtual=True)
        self.receive(record,b"13800138000",chip=0)
        other,other_item=self.put(text="[验收] Windows 来源 → Mac\n🙂",virtual=True)
        received=self.receive(other,other_item.content)
        self.native_mac(received)
        self.faults(record)
        long_text="[验收] 长文字中文🙂\n" + "x" * (65536-len("[验收] 长文字中文🙂\n".encode()))
        long_id,long_item=self.put(text=long_text,favorite=True)
        self.proof={record,long_id}
        self.results["proof_records"]=[{"id":record,"sha256":item.metadata["sha256"]},
                                       {"id":long_id,"sha256":long_item.metadata["sha256"]}]
        self.receive(long_id,long_item.content,virtual=True)
        self.receive(long_id,long_item.content)
        before=self.ble.request({"op":"list"})["records"]
        staged=make_upload(self.config["device_id"],"Mac",text="[验收] 未完成上传")
        self.ble.request({"op":"begin","metadata":staged.metadata})
        self.ble.chunk(staged.metadata["event_id"],"content",0,staged.content[:8])
        self.restart()
        assert self.ble.request({"op":"list"})["records"]==before
        self.receive(long_id,long_item.content)
        self.note("reboot_persistence_and_interrupted_upload", records=len(before))
        self.admin.request({"op":"control","action":"pause","paused":True})
        try: upload(self.ble,staged)
        except HubError as exc: assert exc.code=="paused"
        else: raise AssertionError("pause accepted an upload")
        self.admin.request({"op":"control","action":"pause","paused":False})
        self.admin.request({"op":"control","action":"clear"})
        assert {r["id"] for r in self.ble.request({"op":"list"})["records"]}=={record,long_id}
        self.note("pause_and_clear_preserve_favorites")
        start=self.continuous_start;last=0;iteration=0;uptime=0
        while time.monotonic()-start < self.minutes*60:
            self.heartbeat()
            status=self.ble.request({"op":"status"})
            assert status["uptime_ms"]>=uptime, "unexpected reboot"
            uptime=status["uptime_ms"]
            assert status["records"]<=40 and status["used_bytes"]<=8*1024*1024
            elapsed=time.monotonic()-start
            if elapsed-last>=30 or iteration==0:
                iteration+=1
                r,i=self.put(text=f"[验收] 连续运行 {iteration}，中文与🙂",virtual=iteration%2==0)
                self.receive(r,i.content,virtual=iteration%2!=0)
                if iteration%5==0:
                    self.admin.request({"op":"control","action":"delete","record_id":r})
                if iteration%7==0:
                    self.ble.close();self.ble=BLETransport(self.config)
                    self.sessions[0]=str(uuid.uuid4());self.heartbeat()
                    assert not self.ble.request({"op":"poll"}).get("job")
                self.results["samples"].append({k:status[k] for k in ("uptime_ms","records","used_bytes","free_psram")})
                self.note("continuous_progress",seconds=round(elapsed),iteration=iteration,free_psram=status["free_psram"])
                last=elapsed
            time.sleep(1)
        self.note("continuous_completed",seconds=round(time.monotonic()-start))
        self.success=True

    def faults(self, record):
        self.heartbeat(force=True)
        self.admin.request({"op":"test_select","record_id":record,"kind":0,"target":self.config["device_id"]})
        job=self.ble.request({"op":"poll"})["job"]
        self.ble.request({"op":"ready","job_id":job["id"]})
        time.sleep(2.2)
        try:self.ble.request({"op":"ack","job_id":job["id"],"success":True})
        except HubError as exc:assert exc.code=="expired_delivery"
        else:raise AssertionError("expired write lease was accepted")
        self.admin.request({"op":"test_select","record_id":record,"kind":0,"target":self.config["device_id"]})
        job=self.ble.request({"op":"poll"})["job"]
        self.ble.download(job["id"],0,1)
        self.ble.close();self.ble=BLETransport(self.config)
        self.sessions[0]=str(uuid.uuid4());self.heartbeat(force=True)
        assert not self.ble.request({"op":"poll"}).get("job")
        try:self.ble.request({"op":"ready","job_id":job["id"]})
        except HubError as exc:assert exc.code=="expired_delivery"
        else:raise AssertionError("old delivery survived reconnection")
        self.ble.close()
        try:BLETransport({**self.config,"token":"00"*32})
        except HubError as exc:assert exc.code=="device_authentication_failed"
        else:raise AssertionError("wrong credential authenticated")
        self.ble=BLETransport(self.config);self.heartbeat(force=True)
        for attack in ("replay","tamper"):
            message=self.ble.cipher.encrypt(b'J{"op":"status"}')
            if attack=="replay":
                self.ble.cipher.decrypt(self.ble._submit(self.ble._exchange(message)))
            else:message=message[:-1]+bytes([message[-1]^1])
            try:self.ble._submit(self.ble._exchange(message))
            except (OSError,TimeoutError):pass
            else:raise AssertionError("firmware accepted "+attack)
            self.ble.close();self.ble=BLETransport(self.config)
            self.sessions[0]=str(uuid.uuid4());self.heartbeat(force=True)
        metadata=make_upload(self.config["device_id"],"Mac",text="limit").metadata
        metadata["bytes"]=65537
        try:self.ble.request({"op":"begin","metadata":metadata})
        except HubError as exc:assert exc.code=="too_large"
        else:raise AssertionError("firmware accepted oversized text")
        metadata["kind"]="image"
        try:self.ble.request({"op":"begin","metadata":metadata})
        except HubError as exc:assert exc.code=="invalid_kind"
        else:raise AssertionError("firmware accepted an image")
        self.note("security_disconnect_expiry_limits")

    def native_mac(self,data):
        import sys
        if sys.platform!="darwin": return
        from clipboard_agent.pasteboard import snapshot,restore,read_text
        from clipboard_agent.platform_clipboard import MacClipboard
        clipboard=MacClipboard();previous=snapshot();own_count=None
        try:
            clipboard.write("text",data,str(uuid.uuid4()))
            own_count=clipboard.change_count()
            assert read_text().encode()==data
            assert clipboard.read() is None, "own delivery would be re-uploaded"
        finally:
            if own_count is not None and clipboard.change_count()==own_count:restore(previous)
        self.note("native_mac_clipboard",kind="text",bytes=len(data))

    def close(self):
        if self.ble:self.ble.close()
        for record in self.ids - (self.proof if self.success else set()):
            try:self.admin.request({"op":"control","action":"delete","record_id":record})
            except Exception:pass
        try:self.admin.request({"op":"revoke","device_id":self.peer_id})
        except Exception:pass
        self.link.close()


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--minutes",type=float,default=30)
    parser.add_argument("--report",type=Path,default=Path("backups/v4-ble-acceptance.json"))
    args=parser.parse_args()
    test=Acceptance(load_config(support_dir()/"client-v4.json"),args.minutes,args.report)
    try:test.run()
    except Exception as exc:
        test.note("FAILED",error=type(exc).__name__,detail=str(exc));raise
    finally:test.close()
