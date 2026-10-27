// @vitest-environment node

import { readFileSync, readdirSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import { createParser, defaultOnError } from '@intlify/message-compiler'
import { loadLocaleMessages } from '../../src_assets/common/assets/web/locale.js'

const localeDirectory = resolve(dirname(fileURLToPath(import.meta.url)), '../../src_assets/common/assets/web/public/assets/locale')
const localeFiles = readdirSync(localeDirectory).filter(file => file.endsWith('.json')).sort()

/**
 * @brief Find corrupted numbered placeholders in nested localization messages.
 *
 * @param {object} messages Parsed localization messages.
 * @param {string} [prefix] Parent message key.
 * @return {string[]} Corrupted message keys and their values.
 */
function findCorruptedMessages(messages, prefix = '') {
  return Object.entries(messages).flatMap(([key, value]) => {
    const path = prefix ? `${prefix}.${key}` : key
    if (typeof value === 'string') {
      try {
        createParser({ onError: defaultOnError }).parse(value)
        return /(?:char|format)@@\d+/u.test(value) ? [`${path}: ${value}`] : []
      } catch {
        return [`${path}: ${value}`]
      }
    }
    return findCorruptedMessages(value, path)
  })
}

describe('localization JSON files', () => {
  it('discovers localization catalogs to validate', () => {
    expect(localeFiles.length).toBeGreaterThan(0)
  })

  it.each(localeFiles)('%s exposes only valid messages through the runtime loader', async file => {
    const messages = JSON.parse(readFileSync(resolve(localeDirectory, file), 'utf8'))

    const locale = file.replace(/\.json$/u, '')
    const loaded = await loadLocaleMessages(locale, async () => messages)

    // Catalog updates are maintained separately. Every bundled catalog must be
    // safe to render after the production loader removes malformed messages.
    for (const [loadedLocale, validMessages] of Object.entries(loaded.messages)) {
      expect(findCorruptedMessages(validMessages), `Invalid rendered messages in ${loadedLocale}`).toEqual([])
    }
  })
})
