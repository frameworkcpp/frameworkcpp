import { defineConfig } from 'vitepress'

// https://vitepress.dev/reference/site-config
export default defineConfig({
  title: "FrameworkCpp",
  description: "Build your own Real-Time systems in C++",
  lastUpdated: true,
  head: [
    ['link', { rel: 'preconnect', href: 'https://fonts.googleapis.com' }],
    ['link', { rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: '' }],
    ['link', { rel: 'stylesheet', href: 'https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600&family=Roboto+Slab:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap' }]
  ],
  themeConfig: {
    nav: [
      { text: 'Home', link: '/' },
      { text: 'Documentation', link: '/documentation' }
    ],

    sidebar: [
      {
        text: 'Getting Started',
        link: '/getting-started'
      },
      {
        text: 'Memory',
        items: [
          { text: 'Overview', link: '/memory/' },
          { text: 'The Memory Store', link: '/memory/memory-t' },
          { text: 'Shards', link: '/memory/memory-shard-t' },
          { text: 'Buckets', link: '/memory/memory-bucket' },
          { text: 'Smart Pointers', link: '/memory/smart-pointers' },
          { text: 'Allocators & std::pmr', link: '/memory/allocator' },
          { text: 'Memory Reservation', link: '/memory/reservation' },
          { text: 'Configuration', link: '/memory/configuration' }
        ]
      },
      {
        text: 'Environment',
        items: [
          { text: 'Overview', link: '/environment/' },
          { text: 'Lifecycle', link: '/environment/lifecycle' },
          { text: 'The API', link: '/environment/api' },
          { text: 'Internals', link: '/environment/internals' },
          { text: 'Configuration', link: '/environment/configuration' }
        ]
      },
      {
        text: 'Reactor',
        items: [
          { text: 'Overview', link: '/reactor/' },
          { text: 'The API', link: '/reactor/api' },
          { text: 'Internals', link: '/reactor/internals' },
          { text: 'Configuration', link: '/reactor/configuration' }
        ]
      },
      {
        text: 'Contribute',
        items: [
          { text: 'Documentation', link: '/contribute/documentation' }
        ]
      }
    ],

    socialLinks: [
      { icon: 'github', link: 'https://github.com/frameworkcpp/frameworkcpp' }
    ]
  }
})