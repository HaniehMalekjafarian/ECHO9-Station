
import json
import os
import socket
import sys
import traceback
import subprocess
import time

SOCKET_PATH = os.path.join(os.getenv("AGENTOS_RUNTIME_DIR") or f"/tmp/agentos-{os.geteuid()}", "agentos.sock")


def make_request(request_id, prompt, operation="infer"):
    return {
        "version": 1,
        "request_id": request_id,
        "agent_id": "test-agent",
        "operation": operation,
        "payload": {"prompt": prompt}
    }


def exchange(request, fragmented=False):
    """Start AgentOS, exchange one message, and stop it."""

    server = subprocess.Popen(
        ["./build/agentos"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    client = None

    try:
        # Wait until the server accepts connections.
        deadline = time.monotonic() + 3

        while time.monotonic() < deadline:
            client = socket.socket(
                socket.AF_UNIX, socket.SOCK_STREAM
            )
            client.settimeout(5)

            try:
                client.connect(SOCKET_PATH)
                break
            except OSError:
                client.close()
                client = None

                if server.poll() is not None:
                    raise RuntimeError("AgentOS exited before opening its listening socket.")

                time.sleep(0.02)
        else:
            raise RuntimeError("AgentOS did not accept a connection before the startup deadline.")

        if isinstance(request, bytes):
            message = request
        else:
            message = (
                json.dumps(request) + "\n"
            ).encode("utf-8")

        # Optionally send the message in small pieces.
        if fragmented:
            for i in range(0, len(message), 5):
                client.sendall(message[i:i + 5])
        else:
            client.sendall(message)

        with client.makefile("rb") as stream:
            response_line = stream.readline(16386)

        assert response_line.endswith(b"\n"), (
            "Incomplete response from AgentOS"
        )

        return json.loads(response_line)

    
    finally:
        if client is not None:
            client.close()

        # Persistent AgentOS must be stopped explicitly.
        if server.poll() is None:
            server.terminate()

        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()



def test(name, response, status, request_id, code=None):
    assert response["status"] == status, response
    assert response["request_id"] == request_id, response

    if code is not None:
        assert response["error"]["code"] == code, response

    print(f"PASS: {name}")


def main():
    # 1. Normal request
    response = exchange(
        make_request("test-1", "Check oxygen levels")
    )
    test("Valid request", response, "ok", "test-1")

    assert response["result"]["text"] == (
        "TEST OLLAMA: Oxygen levels are normal."
    )

    # 2. Another prompt: tests the backend fallback
    response = exchange(
        make_request("test-2", "Hello AgentOS")
    )
    test("Different prompt", response, "ok", "test-2")

    assert "Hello AgentOS" in response["result"]["text"]

    # 3. Request transmitted in small chunks
    response = exchange(
        make_request("test-3", "Check reactor"),
        fragmented=True
    )
    test("Fragmented message", response, "ok", "test-3")

    # 4. Invalid JSON
    response = exchange(b'{"version": 1, invalid}\n')
    test(
        "Malformed JSON",
        response, "error", None, "BAD_REQUEST"
    )

    # 5. Missing required field
    request = make_request("test-5", "Hello")
    del request["payload"]["prompt"]

    response = exchange(request)
    test(
        "Missing prompt",
        response, "error", "test-5", "BAD_REQUEST"
    )

    # 6. Unsupported operation
    response = exchange(
        make_request("test-6", "Hello", "delete_everything")
    )
    test(
        "Unsupported operation",
        response, "error", "test-6",
        "UNSUPPORTED_OPERATION"
    )

    # 7. Simulated backend failure
    response = exchange(
        make_request("test-7", "__FAIL_BACKEND__")
    )
    test(
        "Backend failure",
        response, "error", "test-7",
        "BACKEND_ERROR"
    )

    # 8. Message exceeding the protocol limit
    response = exchange(b"x" * 16385 + b"\n")
    test(
        "Oversized message",
        response, "error", None,
        "MESSAGE_TOO_LARGE"
    )


    # 9. Client begins a request but never finishes it.
    response = exchange(
        b'{"version":1,"request_id":"stalled-1"'
    )

    test(
        "Stalled client timeout",
        response,
        "error",
        None,
        "REQUEST_TIMEOUT"
    )

    # 10. Instructor-supplied parser accepts a raw UTF-8 JSON string.
    # The test checks transport correctness, not model-generated wording.
    unicode_request = (json.dumps(
        make_request("test-utf8", "سلام ECHO-9 🌍"),
        ensure_ascii=False,
    ) + "\n").encode("utf-8")
    response = exchange(unicode_request)
    test("UTF-8 prompt", response, "ok", "test-utf8")

    print("\nAll 10 M1 integration tests passed!")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: M1 integration check — {type(error).__name__}: {error}", file=sys.stderr)
        print("Server startup, socket availability, or request/response behavior did not meet the test contract.", file=sys.stderr)
        if os.getenv("AGENTOS_TEST_TRACEBACK") == "1":
            traceback.print_exc()
        sys.exit(1)
