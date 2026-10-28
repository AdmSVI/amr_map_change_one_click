"""Exercise the compiled agent against localhost HTTP/TCP stubs only."""

import hashlib
import http.server
import json
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

EXE = sys.argv.pop(1)
MAP = b'{"header":{"mapName":"TEST"},"advancedPointList":[]}'
MD5 = hashlib.md5(MAP).hexdigest()


class AgentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.claimed = False
        self.reports = []
        self.commands = []
        self.result_code = 200
        self.bad_md5 = False
        self.foreign_url = False
        self.already_current = False
        self.expired = False
        self.process = None
        self.log = self.directory / 'agent.log'
        self.addCleanup(self.stop_agent)
        test = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send(self, status, body):
                self.send_response(status)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                test.assertEqual(self.headers['Authorization'], 'Bearer test-token')
                self.send(200, b'corrupt' if test.bad_md5 else MAP)

            def do_POST(self):
                test.assertEqual(self.headers['Authorization'], 'Bearer test-token')
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                if self.path.endswith('/claim/'):
                    jobs = []
                    if not test.claimed:
                        test.claimed = True
                        origin = 'http://127.0.0.1:1' if test.foreign_url else test.origin
                        jobs = [
                            {
                                'id': 1,
                                'rollout_id': 3,
                                'amr': 'TEST-ROBOT',
                                'ip': '127.0.0.1',
                                'map_md5': MD5,
                                'map_name': 'TEST',
                                'claim_id': 'lease-one',
                                'switch': True,
                                'claim_expires_at': (
                                    datetime.now(timezone.utc)
                                    + timedelta(minutes=-1 if test.expired else 10)
                                ).isoformat(timespec='milliseconds'),
                                'file_url': origin + '/api/v1/map-rollouts/3/file/',
                            }
                        ]
                    self.send(200, json.dumps({'robots': jobs}).encode())
                else:
                    test.reports.append(body)
                    self.send(test.result_code, b'{}')

        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.addCleanup(self.http.server_close)
        self.addCleanup(self.http.shutdown)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        self.origin = f'http://127.0.0.1:{self.http.server_port}'
        self.config = self.directory / 'agent.ini'
        self.config.write_text(
            f'[fleet]\nurl={self.origin}\ntoken=test-token\nallow_http=true\n'
            '[agent]\nconcurrency=1\npoll_seconds=1\nstate_file=state.json\n'
        )

        class Robot(socketserver.BaseRequestHandler):
            def handle(self):
                data = b''
                while len(data) < 16:
                    chunk = self.request.recv(16 - len(data))
                    if not chunk:
                        return
                    data += chunk
                _, _, sequence, length, command, _, _ = struct.unpack('!BBHIHHI', data)
                payload = b''
                while len(payload) < length:
                    payload += self.request.recv(length - len(payload))
                test.commands.append(command)
                if command == 1300:
                    result = {
                        'current_map': 'TEST' if test.already_current else 'OLD',
                        'current_map_md5': MD5 if test.already_current else 'old',
                    }
                elif command == 1020:
                    result = {'task_status': 2}
                else:
                    result = {'ret_code': 1, 'err_msg': 'Unexpected mutation in agent test'}
                body = json.dumps(result).encode()
                self.request.sendall(
                    struct.pack('!BBHIHHI', 0x5A, 1, sequence, len(body), command + 10000, 0, 0)
                    + body
                )

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self.robot = Server(('127.0.0.1', 19204), Robot)
        self.addCleanup(self.robot.server_close)
        self.addCleanup(self.robot.shutdown)
        threading.Thread(target=self.robot.serve_forever, daemon=True).start()

    def start_agent(self):
        self.process = subprocess.Popen(
            [EXE, '--config', str(self.config), '--log-file', str(self.log)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def stop_agent(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)

    def wait_for(self, predicate):
        until = time.monotonic() + 10
        while time.monotonic() < until:
            if predicate():
                return
            if self.process and self.process.poll() is not None:
                self.fail(f'Agent exited early: {self.process.returncode}; {self.log.read_text()}')
            time.sleep(0.03)
        self.fail('Timed out waiting for the compiled agent')

    def state_empty(self):
        try:
            return json.loads((self.directory / 'state.json').read_text())['jobs'] == []
        except (OSError, ValueError):
            return False

    def test_busy_robot_and_claim_identifier(self):
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.assertEqual(self.reports[0]['result'], 'busy')
        self.assertEqual(self.reports[0]['claim_id'], 'lease-one')
        self.assertEqual(self.commands, [1300, 1020])

    def test_already_current_reports_verified_md5(self):
        self.already_current = True
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.assertEqual(self.reports[0]['result'], 'done')
        self.assertEqual(self.reports[0]['map_md5_after'], MD5)
        self.assertEqual(self.commands, [1300])

    def test_download_checksum_failure_never_contacts_robot(self):
        self.bad_md5 = True
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.assertEqual(self.reports[0]['result'], 'failed')
        self.assertEqual(self.commands, [])

    def test_foreign_file_url_is_rejected(self):
        self.foreign_url = True
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.assertEqual(self.reports[0]['result'], 'failed')
        self.assertEqual(self.commands, [])

    def test_unacknowledged_result_is_retried(self):
        self.result_code = 503
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.result_code = 200
        self.wait_for(lambda: len(self.reports) >= 2 and self.state_empty())
        self.assertEqual(self.reports[0], self.reports[1])
        self.assertEqual(self.commands, [1300, 1020])

    def test_restart_resends_result_without_redeploy(self):
        self.result_code = 503
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.stop_agent()
        self.result_code = 200
        self.start_agent()
        self.wait_for(lambda: len(self.reports) >= 2 and self.state_empty())
        self.assertEqual(self.reports[0], self.reports[1])
        self.assertEqual(self.commands, [1300, 1020])

    def test_conflict_drops_old_result(self):
        self.result_code = 409
        self.start_agent()
        self.wait_for(lambda: self.reports and self.state_empty())
        self.assertEqual(len(self.reports), 1)

    def test_expired_claim_never_contacts_robot(self):
        self.expired = True
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.assertEqual(self.reports[0]['result'], 'failed')
        self.assertEqual(self.commands, [])

    def test_restart_reports_unfinished_claim_without_redeploy(self):
        self.claimed = True
        (self.directory / 'state.json').write_text(
            json.dumps(
                {'version': 1, 'jobs': [{'id': '1', 'claim_id': 'lease-one', 'running': True}]}
            )
        )
        self.start_agent()
        self.wait_for(lambda: self.reports)
        self.assertEqual(self.reports[0]['result'], 'failed')
        self.assertEqual(self.commands, [])

    def test_check_config_performs_no_network(self):
        result = subprocess.run(
            [EXE, '--config', str(self.config), '--check-config'], capture_output=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.claimed)
        self.assertEqual(self.commands, [])


if __name__ == '__main__':
    unittest.main()
