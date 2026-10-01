// MCPBridge SMF deploy script — installs the ZHM plugin DLL into Retail/mods
// and enables it in Retail/mods.ini. Runs inside the SMF deploy process.
//
// ModScript interface (simple-mod-framework):
//   analysis(context, modAPI)    — after mod analysis
//   beforeDeploy(context, modAPI)— before this mod deploys
//   afterDeploy(context, modAPI) — after this mod deploys
//   cachingPolicy                — { affected: string[] }
//
// context.config.retailPath = <game>/Retail, context.modRoot = this mod's folder.

// eslint-disable-next-line @typescript-eslint/no-var-requires
const fs = require("fs")
// eslint-disable-next-line @typescript-eslint/no-var-requires
const path = require("path")

export const cachingPolicy = { affected: [] as string[] }

export async function analysis(_context: any, _modAPI: any): Promise<void> {}

export async function beforeDeploy(_context: any, _modAPI: any): Promise<void> {}

export async function afterDeploy(context: any, modAPI: any): Promise<void> {
	const retailPath: string = context.config.retailPath
	const modRoot: string = context.modRoot

	const modsDir = path.join(retailPath, "mods")
	const dllSrc = path.join(modRoot, "MCPBridge.dll")
	const dllDst = path.join(modsDir, "MCPBridge.dll")

	if (!fs.existsSync(dllSrc)) {
		await modAPI.logger.error("MCPBridge.dll missing from mod package")
		return
	}

	fs.mkdirSync(modsDir, { recursive: true })
	fs.copyFileSync(dllSrc, dllDst)

	// Enable the mod in mods.ini if not already present.
	const iniPath = path.join(retailPath, "mods.ini")
	let ini = ""
	if (fs.existsSync(iniPath)) ini = fs.readFileSync(iniPath, "utf8")

	if (!/\[\s*mcpbridge\s*\]/i.test(ini)) {
		fs.writeFileSync(iniPath, ini.replace(/\s*$/, "") + "\r\n\r\n[mcpbridge]\r\n")
	}

	await modAPI.logger.info(
		"MCPBridge installed: Retail/mods/MCPBridge.dll + [mcpbridge] in mods.ini. " +
			"NOTE: disabling this SMF mod will NOT remove the DLL — delete Retail/mods/MCPBridge.dll manually to uninstall."
	)
}
