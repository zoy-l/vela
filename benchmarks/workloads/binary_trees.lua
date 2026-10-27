local function build(depth)
  if depth == 0 then return {} end
  return {left = build(depth - 1), right = build(depth - 1)}
end

local function count(node)
  if node.left == nil or node.right == nil then return 1 end
  return 1 + count(node.left) + count(node.right)
end

return function(input)
  local depth = #input
  local retained = build(depth)
  local checksum = 0
  for iteration = 1, 16 do checksum = checksum + count(build(depth)) end
  return checksum + count(retained)
end
