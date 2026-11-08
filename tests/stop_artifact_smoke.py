#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-loader integration with supplied ARM ELFs; DOES NOT execute ARM/JS.
Usage: python3 tests/stop_artifact_smoke.py /path/JS.ELF /path/GMOS.ELF
"""
import hashlib
import json
import pathlib
import sys
from test_stop import Stop


def main():
    if len(sys.argv)!=3:
        raise SystemExit(__doc__)
    artifacts=[(name,pathlib.Path(arg).resolve()) for name,arg in zip(('JS.ELF','GMOS.ELF'),sys.argv[1:])]
    Stop.setUpClass()
    case=Stop('test_repeated_stop_put_run_and_normal_not_ready')
    records=[]
    try:
        case.setUp()
        for cycle in range(2):
            for name,path in artifacts:
                blob=path.read_bytes()
                case.assertLessEqual(len(blob),1024*1024)
                case.assertIn('OK file',case.put(blob,name))
                case.assertEqual(case.read('/OS/'+name),blob)
                case.assertIn('OK run /OS/'+name,case.command('RUN '+name))
                case.assertTrue(case.c.os_running())
                case.assertEqual(case.command('RUN '+name),'ERR not-ready\n')
                case.assertIn('ERR not-ready',case.put(b'x',name))
                case.assertEqual(case.command('STOP'),'')
                case.assertEqual(case.boundary(),'OK stop\n')
                case.assertFalse(case.c.os_running())
                case.assertEqual(case.c.us_used(),0)
                case.c.tud_cdc_read_flush()  # the fake CDC append buffer is bounded
                records.append({'cycle':cycle,'name':name,'path':str(path),'bytes':len(blob),
                                'sha256':hashlib.sha256(blob).hexdigest(),'put_run_stop':'pass'})
        print(json.dumps({'scope':'real ELF validation/loading and PUTF/RUN/STOP; no ARM, JS or hardware execution',
                          'records':records},indent=2))
    finally:
        case.doCleanups()
        Stop.tearDownClass()


if __name__=='__main__': main()
