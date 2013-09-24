
uniform sampler2D texture;

uniform vec4 tone;

uniform float opacity;
uniform vec4 color;
uniform vec4 flash;

varying vec2 v_texCoord;

const vec3 lumaF = { .299, .587, .114 };

void main()
{
	/* Sample source color */
	vec4 frag = texture2D(texture, v_texCoord);
	
	/* Apply gray */
	const float luma = dot(frag.rgb, lumaF);
	frag.rgb = mix(frag.rgb, vec3(luma), tone.w);
	
	/* Apply tone */
	frag.rgb += tone.rgb;

	/* Apply opacity */
	frag.a *= opacity;
	
	/* Apply color */
	frag.rgb = mix(frag.rgb, color.rgb, color.a);

	/* Apply flash */
	frag.rgb = mix(frag.rgb, flash.rgb, flash.a);
	
	gl_FragColor = frag;
}