-- verdict.lua -- parse-only acceptance probe for the REFERENCE interpreter,
-- the analogue of `luac -p`. Exits 0 if the file loadfile() parses cleanly,
-- 1 if it is a syntax error. loadfile parses WITHOUT executing (a mutant that
-- would loop or throw at runtime still passes if its syntax is valid).
--   lua54ref verdict.lua <path>
local f, err = loadfile(arg[1])
if not f then
  if err and err:find("cannot open") then   -- file-level trouble, not a parse
    os.exit(2)
  end
  os.exit(1)                                -- parse rejected
end
os.exit(0)
