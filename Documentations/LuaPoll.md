# LuaPoll API Reference

Header: `Ring/LuaPoll.h`

`LuaPoll` registers FastRing polling bindings in Lua/LuaJIT state.

## API

```c
void RegisterLuaPoll(struct FastRing* ring, lua_State* state, int handler);
```

## Notes

- `handler` is expected to reference Lua callback/environment used by binding implementation.

