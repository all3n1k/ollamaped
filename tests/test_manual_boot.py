import ast
import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, Mock

def test_manual_boot_scan_never_moves_robot():
    source=(Path(__file__).resolve().parents[1]/'brain/src/main.py').read_text()
    node=next(n for n in ast.parse(source).body if isinstance(n,ast.AsyncFunctionDef)and n.name=='_boot_scan')
    comm=Mock();comm.walk=AsyncMock();comm.stop=AsyncMock();comm.set_behavior=AsyncMock()
    namespace={'AUTONOMY_AUTOSTART':False,'robot_comm':comm}
    exec(compile(ast.Module(body=[node],type_ignores=[]),'main.py','exec'),namespace)
    asyncio.run(namespace['_boot_scan']())
    comm.walk.assert_not_called();comm.stop.assert_not_called();comm.set_behavior.assert_not_called()

def test_autonomy_is_opt_in_by_default():
    source=(Path(__file__).resolve().parents[1]/'brain/src/main.py').read_text()
    assert 'os.getenv("AUTONOMY_AUTOSTART", "0")'in source
