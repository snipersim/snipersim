def get_fp_addsub(f):
  return f["addpd"] + f["addsd"] + f["addss"] + f["addps"] + f["subpd"] + f["subsd"] + f["subss"] + f["subps"]

def get_fp_muldiv(f):
  return f["mulpd"] + f["mulsd"] + f["mulss"] + f["mulps"] + f["divpd"] + f["divsd"] + f["divss"] + f["divps"]
