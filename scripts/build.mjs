import {rollup} from "rollup"
import config from "../rollup.config.js"

const configs = Array.isArray(config) ? config : [config]

try {
  for (const rollupConfig of configs) {
    const bundle = await rollup(rollupConfig)
    const outputs = Array.isArray(rollupConfig.output) ? rollupConfig.output : [rollupConfig.output]

    for (const output of outputs)
      await bundle.write(output)

    await bundle.close()
  }
  process.exit(0)
} catch (error) {
  console.error(error)
  process.exit(1)
}
