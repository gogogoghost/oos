export function invoke(operation, request) {
  if (operation !== "echo")
    throw new TypeError("unsupported module operation");
  return new Uint8Array(request);
}
