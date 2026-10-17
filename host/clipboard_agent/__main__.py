from clipboard_agent.hub_client import main
from clipboard_agent.hub_protocol import HubError, error_message
import sys

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HubError as exc:
        print("错误：" + error_message(exc.code), file=sys.stderr)
        raise SystemExit(1)
    except (OSError, TimeoutError) as exc:
        print("无法连接开发板：" + type(exc).__name__ + "。请检查蓝牙开关、系统蓝牙权限和开发板供电。", file=sys.stderr)
        raise SystemExit(1)
