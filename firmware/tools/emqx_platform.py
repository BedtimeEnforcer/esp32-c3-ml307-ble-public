"""EMQX China official platform API. Credentials are read from a private file.
No account password is needed. No credential values are printed.
Only a Serverless deployment can be created, using the documented default
zero spending limit. Never creates a dedicated/paid deployment.
"""
import argparse
import base64
import json
from pathlib import Path
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / 'private'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def credentials():
    value = json.loads((PRIVATE / 'emqx-platform.local.json').read_text(encoding='utf-8-sig'))
    parsed = urllib.parse.urlsplit(value['endpoint'])
    if parsed.scheme != 'https' or parsed.hostname != 'cloud.emqx.com' or parsed.port not in (None, 443) or parsed.path.rstrip('/') != '/public_api/v1' or parsed.query or parsed.username:
        raise ValueError('仅允许官方中国区平台 API https://cloud.emqx.com/public_api/v1')
    if not value.get('key') or not value.get('secret') or any('替换' in value[k] for k in ('key', 'secret')):
        raise ValueError('临时 API 凭据尚未填写')
    return value


def request(value, path, body=None):
    auth = base64.b64encode((value['key'] + ':' + value['secret']).encode()).decode()
    req = urllib.request.Request(value['endpoint'].rstrip('/') + path,
                                 data=None if body is None else json.dumps(body).encode(),
                                 headers={'Authorization': 'Basic ' + auth, 'Content-Type': 'application/json', 'Accept': 'application/json'})
    try:
        with urllib.request.build_opener(NoRedirect()).open(req, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        # Do not echo server error bodies: some services reflect credentials.
        code = error.code
        raise RuntimeError(f'EMQX API HTTP {code}: ' + {401: 'API凭据未通过验证', 403: 'API范围或项目权限不足', 429: '请求频率受限', 422: '参数不被接受'}.get(code, '请求未完成')) from None


def items(result):
    if isinstance(result, list):
        return result
    if isinstance(result, dict) and isinstance(result.get('data'), list):
        return result['data']
    raise ValueError('平台返回的列表结构与官方文档不同，停止写操作')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--create', action='store_true')
    parser.add_argument('--details', metavar='DEPLOYMENT_ID')
    args = parser.parse_args()
    value = credentials()
    if args.details:
        if not args.details.isalnum():
            raise ValueError('部署编号非法')
        detail = request(value, '/deployments/' + args.details)
        (PRIVATE / 'emqx-deployment.local.json').write_text(json.dumps(detail, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(detail, ensure_ascii=False, indent=2))
        return
    projects = items(request(value, '/projects'))
    deployments = items(request(value, '/deployments'))
    discovery = {'projects': projects, 'deployments': deployments}
    (PRIVATE / 'emqx-discovery.local.json').write_text(json.dumps(discovery, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'projects': [{k: p.get(k) for k in ('projectID', 'projectName')} for p in projects],
                      'deployments': [{k: d.get(k) for k in ('deploymentID', 'deploymentName', 'deploymentType', 'region', 'status')} for d in deployments]}, ensure_ascii=False, indent=2))
    if not args.create:
        return
    if deployments:
        raise ValueError('账号已有部署，先检查并复用；不重复创建')
    if not projects:
        raise ValueError('未找到有权限的项目')
    project = next((p for p in projects if p.get('projectName') == 'default' or p.get('projectID') == 'default'), projects[0])
    plan = {'platform': 'aliyun', 'region': 'cn-hangzhou', 'connections': 1000, 'tps': 1000,
            'deploymentType': 'serverless', 'deploymentName': 'btclone-prototype',
            'deploymentVersion': 'v5', 'projectID': project['projectID'], 'freeTrial': False}
    (PRIVATE / 'emqx-create-plan.local.json').write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding='utf-8')
    result = request(value, '/deployments', plan)
    (PRIVATE / 'emqx-create-result.local.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: result.get(k) for k in ('deploymentID', 'deploymentType', 'deploymentName', 'status', 'region')}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(type(error).__name__ + ': ' + str(error))
        raise SystemExit(1)
