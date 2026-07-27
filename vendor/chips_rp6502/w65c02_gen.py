#-------------------------------------------------------------------------------
#   w65c02_gen.py
#   Regenerate the overriding chips/chips/w65c02.h decoder next to this script.
#
#   Carries only the delta from vendor/chips: the upstream generator is imported
#   and its BBR/BBS emitter replaced, so every other opcode tracks upstream.
#
#   BBR/BBS: upstream emits a flat 6 cycles with a dummy WRITE of the zero page
#   value. The W65C02S does neither. SingleStepTests/65x02 wdc65c02/v1/0f.json
#   (10,000 cases) is all reads, 56,331 of them, and the cycle count varies with
#   the branch: 5 not taken, 6 taken, 7 taken across a page. Upstream's cited
#   source, SingleStepTests/ProcessorTests, is archived and predates the July
#   2024 correction.
#-------------------------------------------------------------------------------
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VENDOR_CODEGEN = os.path.abspath(os.path.join(HERE, '..', 'chips', 'codegen'))

# Loaded by path under a name of its own: this file shares the vendored file's
# name, so a plain import would find itself. src/fpga/codegen imports this to
# generate RTL from the same corrected tables.
sys.path.insert(0, VENDOR_CODEGEN)  # for the vendored generator's own imports
_spec = importlib.util.spec_from_file_location(
    'w65c02_gen_vendored', os.path.join(VENDOR_CODEGEN, 'w65c02_gen.py'))
up = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = up
_spec.loader.exec_module(up)

#-------------------------------------------------------------------------------
#   Read the zero page byte, re-read it with the address held, fetch the
#   relative operand, then spend a cycle at PC per branch step taken. The final
#   tick gets its _FETCH() appended by enc_op, as upstream expects of a SELF op.
#-------------------------------------------------------------------------------
def _bitbr(bit, taken):
    def i(o):
        o.t('_SA(c->PC++);')
        o.t('c->AD=_GD();_SA(c->AD);')
        o.t('c->AD=_GD();_SA(_GA());')
        o.t('c->AD=(c->AD>>%d)&1;_SA(c->PC++);' % bit)
        o.t('if(%s(uint8_t)c->AD){c->AD=c->PC+(int8_t)_GD();_SA(c->PC);}else{_FETCH();}' % taken)
        o.t('if((c->AD&0xFF00)==(c->PC&0xFF00)){c->PC=c->AD;_FETCH();}else{_SA(c->PC);}')
        o.t('c->PC=c->AD;')
    return i

for _b in range(8):
    up.EMIT['BBR%d' % _b] = _bitbr(_b, '0==')
    up.EMIT['BBS%d' % _b] = _bitbr(_b, '0!=')

if __name__ == '__main__':
    up.INOUT_PATH = os.path.join(HERE, 'chips', 'chips', 'w65c02.h')
    up.out_lines = ''
    for op in range(0, 256):
        up.write_op(up.enc_op(op))
    up.write_result()
