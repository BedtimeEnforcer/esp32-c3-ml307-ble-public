"""Direct DNS/TCP/TLS/HTTPS probe. Bypasses environment HTTP proxies.
This cannot by itself rule out a system TUN or an upstream proxy.
Use --mqtt-host after creating a private deployment to test its actual port.
"""
import argparse
import concurrent.futures
import json
import socket
import ssl
import time
import urllib.request


def probe(host, port=443, https=True):
    start = time.monotonic()
    result = {'host': host, 'port': port, 'http_proxy_used': False}
    try:
        result['addresses'] = sorted({a[4][0] for a in socket.getaddrinfo(host, port)})
        with socket.create_connection((host, port), timeout=10) as raw:
            with ssl.create_default_context().wrap_socket(raw, server_hostname=host) as tls:
                result['tls'] = tls.version()
                result['certificate_verified'] = True
        if https:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            try:
                with opener.open('https://' + host, timeout=12) as response:
                    result['http_status'], result['final_url'] = response.status, response.url
            except urllib.error.HTTPError as error:
                result['http_status'] = error.code
        result['ok'] = True
    except Exception as error:
        result['ok'] = False
        result['error'] = str(error)
    result['seconds'] = round(time.monotonic() - start, 2)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--mqtt-host')
    parser.add_argument('--port', type=int, default=8883)
    parser.add_argument('--output')
    args = parser.parse_args()
    targets = [(args.mqtt_host, args.port, False)] if args.mqtt_host else [
        (host, 443, True) for host in ('accounts-zh.emqx.com', 'cloud.emqx.com', 'www.emqx.com', 'accounts.emqx.com')]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda values: probe(*values), targets))
    report = {'note': 'HTTP proxy bypassed; system TUN/upstream routing not verified; not a mobile-network acceptance test', 'results': results}
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.output:
        from pathlib import Path
        Path(args.output).write_text(text, encoding='utf-8')
