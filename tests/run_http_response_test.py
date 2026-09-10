#!/usr/bin/env python3
"""Read production control responses using a standard persistent HTTP client."""
import http.client
from pathlib import Path
import socket
import subprocess
import sys
import threading

binary = str(Path(sys.argv[1]).resolve(strict=True))
replies = [subprocess.check_output([binary, case]) for case in ("empty", "body")]
failures = []
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(3)

    def serve():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(3)
                for response in replies:
                    request = b""
                    while b"\r\n\r\n" not in request:
                        part = peer.recv(1024)
                        if not part:
                            raise AssertionError("Client closed before second request")
                        request += part
                    peer.sendall(response)
        except Exception as error:
            failures.append(error)

    worker = threading.Thread(target=serve)
    worker.start()
    client = http.client.HTTPConnection(*listener.getsockname(), timeout=1)
    try:
        client.request("POST", "/play", body=b"")
        response = client.getresponse()
        assert response.status == 200
        assert response.read() == b""
        first_socket = client.sock
        assert first_socket is not None
        client.request("GET", "/playback-info")
        response = client.getresponse()
        assert response.read() == b"OK"
        assert client.sock is first_socket
    finally:
        client.close()
        worker.join(timeout=4)
    assert not worker.is_alive()
    assert not failures, failures
print("Empty control response completes immediately; same HTTP connection handles the next request.")
