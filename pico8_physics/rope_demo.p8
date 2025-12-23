pico-8 cartridge // http://www.pico-8.com
version 29
__lua__
-- rope physics demo
-- by antigravity for user

-- constants
grav = 0.15
fric = 0.95 -- air resistance
bounce = 0.7
rope_seg_len = 5
rope_segs = 12
k_stiff = 0.8 -- stiffness of rope (1=rigid)

-- colors
c_bg = 0
c_rope = 6
c_highlight = 7
cols = {8, 9, 10, 11, 12, 13, 14}

-- state
shapes = {} -- active objects
preview_shape = nil
active_shape_idx = -1
cam_shake = 0

function _init()
	srand(time())
	randomize_preview()
end

function _update60()
	-- input handling
	
	-- o button (z): new random shape
	if (btnp(4)) randomize_preview()
	
	-- x button (x): spawn shape
	if (btnp(5)) spawn_shape()
	
	-- arrow/dpad: swing active shape
	if active_shape_idx > 0 then
		local s = shapes[active_shape_idx]
		if s then
			local force = 0.2
			if (btn(0)) s.vx -= force
			if (btn(1)) s.vx += force
		end
	end

	-- physics update
	update_physics()
	
	-- camera shake decay
	if (cam_shake > 0) cam_shake -= 1
end

function _draw()
	cls(c_bg)
	
	-- draw ropes & shapes
	for s in all(shapes) do
		draw_rope(s)
		draw_shape(s)
	end
	
	-- draw preview
	rectfill(0,0,127,16,1)
	print("preview:", 4, 6, 7)
	if preview_shape then
		local px, py = 45, 8
		draw_poly(px, py, preview_shape.sides, preview_shape.r, preview_shape.col)
	end
	print("x:spawn o:change", 60, 6, 6)
	
	-- highlight active
	if active_shape_idx > 0 and shapes[active_shape_idx] then
		local s = shapes[active_shape_idx]
		-- draw simple indicator above the active one
		local hx = s.x
		local hy = s.y - s.r - 4
		line(hx-2, hy, hx, hy+2, 7)
		line(hx, hy+2, hx+2, hy, 7)
	end
end

-- logic helpers

function randomize_preview()
	preview_shape = {
		sides = flr(rnd(4)) + 3, -- 3 to 6 sides
		r = flr(rnd(6)) + 6,     -- radius 6 to 11
		col = cols[flr(rnd(#cols))+1],
		x = 64, -- spawn x (temp)
		y = 0   -- spawn y
	}
end

function spawn_shape()
	if not preview_shape then return end
	
	local new_s = {
		x = flr(rnd(100)) + 14,
		y = -10,
		vx = 0, vy = 0,
		radius = preview_shape.r, -- radius for physics
		r = preview_shape.r,      -- radius for draw
		sides = preview_shape.sides,
		col = preview_shape.col,
		-- rope init
		rope = {}
	}
	
	-- anchor point
	new_s.anchor_x = new_s.x
	new_s.anchor_y = 0
	
	-- init rope segments
	for i=1,rope_segs do
		add(new_s.rope, {
			x=new_s.x, 
			y=new_s.anchor_y + i*2, -- start slightly bunchy
			oldx=new_s.x, 
			oldy=new_s.anchor_y + i*2
		})
	end
	
	-- attach shape to last segment
	local last = new_s.rope[#new_s.rope]
	new_s.x = last.x
	new_s.y = last.y
	new_s.oldx = new_s.x
	new_s.oldy = new_s.y
	
	add(shapes, new_s)
	active_shape_idx = #shapes
	
	randomize_preview()
end

-- physics engine

function update_physics()
	for i,s in pairs(shapes) do
		
		-- 1. verlet interaction for rope
		-- anchor point is static at s.anchor_x, s.anchor_y
		
		local prev_node = {x=s.anchor_x, y=s.anchor_y, fixed=true}
		
		for j,node in pairs(s.rope) do
			-- apply gravity & velocity
			local vx = (node.x - node.oldx) * fric
			local vy = (node.y - node.oldy) * fric
			
			node.oldx = node.x
			node.oldy = node.y
			
			if not node.fixed then
				node.x += vx
				node.y += vy + grav
			end
			
			-- constrain to previous node (distance constraint)
			local dx = node.x - prev_node.x
			local dy = node.y - prev_node.y
			local dist = sqrt(dx*dx + dy*dy)
			local diff = dist - rope_seg_len
			
			if dist > 0.1 then -- avoid div by zero
				local correction = (diff / dist) * 0.5 * k_stiff
				
				if not prev_node.fixed then
					prev_node.x += dx * correction
					prev_node.y += dy * correction
				end
				if not node.fixed then
					node.x -= dx * correction
					node.y -= dy * correction
				end
			end
			
			prev_node = node
		end
		
		-- 2. update shape body (attached to last rope node)
		-- shape acts as a heavy weight at end of rope
		
		local last = s.rope[#s.rope]
		
		-- sync shape pos with last node (soft attach or hard?)
		-- let's make the shape *be* the last node physics-wise but heavier?
		-- simpler: shape physics is driven by verlet too
		
		-- actually, let's treat the shape as a separate body that 
		-- constrains the last rope node
		
		-- apply forces to shape
		local vx = (s.x - s.oldx) * fric + s.vx
		local vy = (s.y - s.oldy) * fric + s.vy
		s.oldx = s.x
		s.oldy = s.y
		s.x += vx
		s.y += vy + grav
		
		-- reset temp forces
		s.vx = 0
		s.vy = 0
		
		-- constrain shape to last rope node
		local dx = s.x - last.x
		local dy = s.y - last.y
		local dist = sqrt(dx*dx + dy*dy)
		local diff = dist - 2 -- shape attachment distance
		if dist > 0.1 then
			local correction = (diff/dist) * 0.5
			last.x += dx * correction
			last.y += dy * correction
			s.x -= dx * correction
			s.y -= dy * correction
		end
		
		-- 3. collisions with other shapes
		for k,other in pairs(shapes) do
			if i != k then
				check_collision(s, other)
			end
		end
		
		-- 4. screen bounds (bounce)
		if s.x < s.r then 
			s.x = s.r 
			local d = s.x - s.oldx
			s.oldx = s.x + d*bounce
		end
		if s.x > 128-s.r then 
			s.x = 128-s.r
			local d = s.x - s.oldx
			s.oldx = s.x + d*bounce
		end
	end
end

function check_collision(a, b)
	local dx = a.x - b.x
	local dy = a.y - b.y
	local dist = sqrt(dx*dx + dy*dy)
	local min_dist = a.r + b.r
	
	if dist < min_dist and dist > 0 then
		-- resolve overlap
		local overlap = min_dist - dist
		local nx = dx / dist
		local ny = dy / dist
		
		-- separate
		local sep_x = nx * overlap * 0.5
		local sep_y = ny * overlap * 0.5
		
		a.x += sep_x
		a.y += sep_y
		b.x -= sep_x
		b.y -= sep_y
		
		-- exchange momentum (simple)
		-- in verlet, we modify position to change velocity
		-- let's just push them apart, Verlet handles the "bounce" naturally 
		-- via position change if we do enough easing.
		-- but to add "bounce" we can modify old_pos slightly to reflect energy conservation?
		-- for now, position projection (separation) is usually enough for stack stability
	end
end

-- drawing

function draw_rope(s)
	local px, py = s.anchor_x, s.anchor_y
	for node in all(s.rope) do
		line(px, py, node.x, node.y, c_rope)
		px, py = node.x, node.y
	end
	line(px, py, s.x, s.y, c_rope)
end

function draw_shape(s)
	local is_active = (s == shapes[active_shape_idx])
	local c = s.col
	local r = s.r
	
	-- draw poly
	draw_poly(s.x, s.y, s.sides, r, c)
	
	-- highlight stroke
	if is_active then
		-- draw simplified white outline logic or just markers
		circ(s.x, s.y, 2, 7)
	end
end

function draw_poly(x, y, sides, r, col)
	local step = 1/sides
	local ang = 0.25 -- start upright
	
	local x0, y0
	local first_x, first_y
	
	for i=1,sides do
		local a = ang * 3.14159 * 2
		local px = x + cos(a)*r
		local py = y + sin(a)*r
		
		if i>1 then
			line(x0, y0, px, py, col)
		else
			first_x = px
			first_y = py
		end
		
		x0, y0 = px, py
		ang += step
	end
	line(x0, y0, first_x, first_y, col)
	
	-- fill? nah, wireframe is cool, maybe chaotic fill
	-- simple fill pattern inner
	if r > 4 then
		fillp(0b1010010110100101)
		circfill(x,y,r-2,col)
		fillp()
	end
end
