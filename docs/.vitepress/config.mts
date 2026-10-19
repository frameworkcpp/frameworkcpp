import { defineConfig } from 'vitepress'

// https://vitepress.dev/reference/site-config
export default defineConfig({
  title: "FrameworkCpp",
  description: "Build your own Real-Time systems in C++",
  lastUpdated: true,
  head: [
    ['link', { rel: 'preconnect', href: 'https://fonts.googleapis.com' }],
    ['link', { rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: '' }],
    ['link', { rel: 'stylesheet', href: 'https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap' }]
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
        text: 'Memory Management',
        items: [
          { text: 'Overview', link: '/memory-management/' },
          { text: 'The Arena', link: '/memory-management/arena-t' },
          { text: 'Shards', link: '/memory-management/arena-shard-t' },
          { text: 'Buckets', link: '/memory-management/arena-bucket' },
          { text: 'Smart Pointers', link: '/memory-management/smart-pointers' },
          { text: 'Allocators & std::pmr', link: '/memory-management/allocator' },
          { text: 'Configuration', link: '/memory-management/configuration' }
        ]
      },
      {
        text: 'Environment Management',
        items: [
          { text: 'Overview', link: '/environment/' },
          { text: 'Lifecycle', link: '/environment/lifecycle' },
          { text: 'The API', link: '/environment/api' },
          { text: 'Internals', link: '/environment/internals' },
          { text: 'Configuration', link: '/environment/configuration' }
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