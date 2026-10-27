local floor = math.floor
local Particle = {}
Particle.__index = Particle

function Particle:update()
  self.x, self.y = self.x + self.vx, self.y + self.vy
  if self.x > 100 then self.x, self.vx = 200 - self.x, -self.vx end
  if self.x < 0 then self.x, self.vx = -self.x, -self.vx end
  if self.y > 100 then self.y, self.vy = 200 - self.y, -self.vy end
  if self.y < 0 then self.y, self.vy = -self.y, -self.vy end
end

return function(input)
  local particles = {}
  for index = 1, #input do
    local value = input[index]
    particles[index] = setmetatable({
      x = value - floor(value / 97) * 97,
      y = value - floor(value / 37) * 37,
      vx = 0.25,
      vy = -0.125,
    }, Particle)
  end
  for frame = 1, 64 do
    for index = 1, #particles do particles[index]:update() end
  end
  local checksum = 0
  for index = 1, #particles do
    local particle = particles[index]
    checksum = checksum + floor(particle.x * 1000) + floor(particle.y * 1000)
  end
  return checksum
end
