local SpawnerScriptSample = 
{
	Properties =
	{
		ScriptVar = { default = "test3" }
	}
}

function SpawnerScriptSample:OnActivate()
    -- Register our handlers to receive notification from the spawner attached to this entity.
    if( self.spawnerNotiBusHandler == nil ) then
        self.spawnerNotiBusHandler = SpawnerComponentNotificationBus.Connect(self, self.entityId)
    end
    Script.ReloadScript("some/path/test3.lua")
end

-- This handler is called when we start spawning a slice.
function SpawnerScriptSample:OnSpawnBegin(sliceTicket)
    -- Do something so we know if/when this is being called
    Debug.Log("Slice Spawn Begin")
end

-- This handler is called when we're finished spawning a slice.
function SpawnerScriptSample:OnSpawnEnd(sliceTicket)
    -- Do something so we know if/when this is being called
    Debug.Log("Slice Spawn End")
end

-- This handler is called whenever an entity is spawned.
function SpawnerScriptSample:OnEntitySpawned(sliceTicket, entityId)
    -- Do something so we know if/when this is being called
    Debug.Log("Entity Spawned: " .. tostring(entityId) )
end

function SpawnerScriptSample:OnDeactivate()
    -- Disconnect our spawner notificaton 
    if self.spawnerNotiBusHandler ~= nil then
        self.spawnerNotiBusHandler:Disconnect()
        self.spawnerNotiBusHandler = nil
    end
end

return SpawnerScriptSample