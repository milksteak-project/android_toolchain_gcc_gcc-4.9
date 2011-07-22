-- { dg-do compile { target *-*-linux* } }
-- { dg-options "-gdwarf-2 -cargs -dA" }

package Debug1 is

  function N return Integer;
  pragma Import (Ada, N);

  type Arr is array (-N .. N) of Boolean;
  A : Arr;

end Debug1;

-- { dg-final { scan-assembler-times "byte\t0x1\t# DW_AT_artificial" 4 } }
