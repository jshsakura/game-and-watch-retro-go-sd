#!/usr/bin/env python3
"""Use gnwmanager without its automatic kill of an unrelated OpenOCD session."""
import subprocess


def refuse_existing(name):
    if name != 'openocd':
        raise RuntimeError('unexpected backend cleanup request: '+name)
    # One admission check; no retries or monitoring of the existing process.
    r=subprocess.run(['pgrep','-x','openocd'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    if r.returncode == 0:
        raise RuntimeError('another OpenOCD process exists; refusing to terminate it')
    if r.returncode != 1:
        raise RuntimeError('cannot establish debugger ownership')


def main():
    import gnwmanager.ocdbackend.openocd_backend as backend
    from gnwmanager.cli.main import run_app
    backend.kill_processes_by_name=refuse_existing
    run_app()

if __name__=='__main__':main()
