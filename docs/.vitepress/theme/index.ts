// SPDX-License-Identifier: AGPL-3.0-only

import { h, nextTick, watch } from 'vue'
import type { Theme } from 'vitepress'
import { useData } from 'vitepress'
import DefaultTheme from 'vitepress/theme'
import { createMermaidRenderer } from 'vitepress-mermaid-renderer'
import 'vitepress-mermaid-renderer/css'

import './custom.css'

// Mermaid is themed to match the site palette instead of shipping one of its
// stock themes, so diagrams read as part of the page in both modes.
const lightDiagramConfig = {
  theme: 'base',
  themeVariables: {
    fontFamily: "'Inter', sans-serif",
    fontSize: '15px',
    background: '#ffffff',
    primaryColor: '#e3f2ef',
    primaryTextColor: '#2b2b31',
    primaryBorderColor: '#0f766e',
    lineColor: '#5d6167',
    secondaryColor: '#f7f7f5',
    tertiaryColor: '#f7f7f5',
    clusterBkg: '#f7f7f5',
    clusterBorder: '#d0d0ca',
    edgeLabelBackground: '#ffffff'
  }
} as const

const darkDiagramConfig = {
  theme: 'base',
  themeVariables: {
    fontFamily: "'Inter', sans-serif",
    fontSize: '15px',
    background: '#141619',
    primaryColor: '#123b38',
    primaryTextColor: '#e8e8e2',
    primaryBorderColor: '#5eead4',
    lineColor: '#9aa0a6',
    secondaryColor: '#1b1e23',
    tertiaryColor: '#1b1e23',
    clusterBkg: '#1b1e23',
    clusterBorder: '#3b4149',
    edgeLabelBackground: '#141619'
  }
} as const

export default {
  extends: DefaultTheme,
  Layout: () => {
    const { isDark } = useData()

    const initMermaid = () => {
      createMermaidRenderer(
        isDark.value ? darkDiagramConfig : lightDiagramConfig
      ).setToolbar({
        showLanguageLabel: false,
        downloadFormat: 'svg',
        fullscreenMode: 'dialog',
        desktop: {
          zoomIn: 'enabled',
          zoomOut: 'enabled',
          resetView: 'enabled',
          zoomLevel: 'enabled',
          copyCode: 'enabled',
          download: 'enabled',
          toggleFullscreen: 'enabled'
        }
      })
    }

    nextTick(() => initMermaid())
    watch(
      () => isDark.value,
      () => initMermaid()
    )

    return h(DefaultTheme.Layout)
  }
} satisfies Theme
